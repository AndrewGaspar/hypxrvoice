#include "doctest.h"

#include "HudClient.hpp"
#include "HudModel.hpp"

#include <string>

// WP-H8: rasterisation, the fade-opacity envelope, and the HUD IPC wire format moved to
// the shared hypxrhud daemon; hypxrvoice is a pure D-Bus client. These tests cover what
// hypxrvoice still owns: the pure SHudView builders (phrasing, colour roles, layout) and
// the SHudView -> hypxrhud panel-props mapping (HudClient::hudPropsFromView). The
// create/update/dismiss round-trip against the real daemon lives in test_hud_dbus.cpp.

namespace {
    // Does the view contain a line whose text equals `t`?
    bool hasLine(const SHudView& v, const std::string& t) {
        for (auto& l : v.lines)
            if (l.text == t)
                return true;
        return false;
    }
    bool hasLineColor(const SHudView& v, EHudColor c) {
        for (auto& l : v.lines)
            if (l.color == c)
                return true;
        return false;
    }
    bool propsHasLine(const SHudProps& p, const std::string& t) {
        for (auto& l : p.lines)
            if (l.text == t)
                return true;
        return false;
    }
}

// ---- pure view builders ---------------------------------------------------------------

TEST_CASE("hud: action phrase is terse and verb-appropriate") {
    SAction a;
    a.verb = EVerb::Anchor; a.target = "XR-code";
    CHECK(hudActionPhrase(a) == "anchoring XR-code");

    SAction m; m.verb = EVerb::MoveDist; m.target = "XR-web"; m.deltaM = -0.25;
    CHECK(hudActionPhrase(m) == "closer — XR-web");
    m.deltaM = 0.25;
    CHECK(hudActionPhrase(m) == "further — XR-web");

    SAction l; l.verb = EVerb::LaunchApp; l.app = "browser";
    CHECK(hudActionPhrase(l) == "opening browser");

    SAction act; act.verb = EVerb::Center; act.target = "active";
    CHECK(hudActionPhrase(act) == "centering"); // implicit "active" is not appended

    SAction view; view.verb = EVerb::MonitorView; view.sub = "off";
    CHECK(hudActionPhrase(view) == "hiding monitor view");
    view.sub = "on";
    CHECK(hudActionPhrase(view) == "showing monitor view");
    view.sub = "toggle";
    CHECK(hudActionPhrase(view) == "toggling monitor view");

    SAction c; c.verb = EVerb::Clarify; c.clarifyQuestion = "which firefox?";
    CHECK(hudActionPhrase(c) == "which firefox?");
}

TEST_CASE("hud: listening panel persists (no auto-fade) and shows partial text") {
    SConfig cfg;
    SHudView v = hudForListening("open the browser", cfg);
    CHECK(v.state == EHudState::Listening);
    CHECK(v.holdMs < 0); // persistent until replaced (daemon treats hold<0 as persistent)
    REQUIRE(v.lines.size() >= 2);
    CHECK(v.lines.front().big);
    CHECK(hasLine(v, "open the browser"));
}

TEST_CASE("hud: actionable command builds an Action panel with title + confidence") {
    SConfig cfg; // executor.dryRun default true
    SAction a; a.verb = EVerb::Anchor; a.target = "XR-code"; a.confidence = 0.82;
    a.targetSource = ETargetSource::Named;
    SExecPlan plan; plan.ok = true; plan.steps.push_back({{"hyprctl","openxr","anchor","XR-code","local"}, "x"});

    SHudView v = hudForAction(a, plan, cfg);
    CHECK(v.state == EHudState::Action);
    CHECK(v.lines.front().text == "anchoring XR-code");
    CHECK(v.lines.front().color == EHudColor::Accent);
    CHECK(v.confidence == doctest::Approx(0.82f));
    CHECK(v.dryRun);
    CHECK(hasLine(v, "dry-run"));
    CHECK(v.holdMs == 2600); // transient dwell default (forwarded as a hold_ms prop)
}

TEST_CASE("hud: gaze deixis surfaces a provenance hint") {
    SConfig cfg;
    SAction a; a.verb = EVerb::Pick; a.target = "XR-web"; a.targetSource = ETargetSource::Deixis;
    a.gaze.stable = true;
    SExecPlan plan; plan.ok = true;
    SHudView v = hudForAction(a, plan, cfg);
    CHECK(hasLine(v, "you looked at it"));

    a.gaze.stable = false;
    SHudView v2 = hudForAction(a, plan, cfg);
    CHECK(hasLine(v2, "best-guess gaze"));
    CHECK(hasLineColor(v2, EHudColor::Warn)); // uncertain gaze flagged
}

TEST_CASE("hud: clarify builds a question + candidate list") {
    SConfig cfg;
    SAction a; a.verb = EVerb::Clarify; a.clarifyQuestion = "which one?";
    a.clarifyCandidates = {"XR-web", "XR-chat"};
    SExecPlan plan; // ok stays false but Clarify short-circuits before the refusal path
    SHudView v = hudForAction(a, plan, cfg);
    CHECK(v.state == EHudState::Clarify);
    CHECK(v.lines.front().text == "which one?");
    CHECK(hasLine(v, "• XR-web"));
    CHECK(hasLine(v, "• XR-chat"));
    CHECK(v.holdMs >= 3500); // a question gets a longer dwell
}

TEST_CASE("hud: a refused plan becomes an Error panel carrying the reason") {
    SConfig cfg;
    SAction a; a.verb = EVerb::Anchor; a.target = "XR-ghost"; a.targetSource = ETargetSource::Named;
    SExecPlan plan; plan.ok = false; plan.reason = "no such monitor";
    SHudView v = hudForAction(a, plan, cfg);
    CHECK(v.state == EHudState::Error);
    CHECK(v.lines.front().color == EHudColor::Bad);
    CHECK(hasLine(v, "no such monitor"));
}

TEST_CASE("hud: approximated plan is flagged") {
    SConfig cfg;
    SAction a; a.verb = EVerb::Pick; a.target = "XR-code";
    SExecPlan plan; plan.ok = true; plan.approximated = true;
    SHudView v = hudForAction(a, plan, cfg);
    CHECK(v.approximated);
    CHECK(hasLine(v, "approx"));
}

TEST_CASE("hud: none verb yields a hidden panel") {
    // hudForAction is pure and has no transcript to echo, so it still answers Hidden; the
    // rejection panel is built by the caller from the utterance (hudForRejection below).
    SConfig cfg;
    SAction a; a.verb = EVerb::None;
    SExecPlan plan;
    SHudView v = hudForAction(a, plan, cfg);
    CHECK(v.state == EHudState::Hidden);
}

// ---- rejection panels (WP-V6: never leave a window with no feedback) --------------------

TEST_CASE("hud: an unparseable transcript is echoed back, never hidden") {
    SConfig  cfg;
    SHudView v = hudForRejection("put the editor here", "", cfg);

    CHECK(v.state == EHudState::Rejected);
    CHECK(v.state != EHudState::Hidden); // the bug: silence was indistinguishable from a dead mic
    CHECK_FALSE(v.empty());
    REQUIRE(v.lines.size() == 2);
    // What we heard, verbatim, as the title — proof the mic and ASR worked.
    CHECK(v.lines.front().text == "put the editor here");
    CHECK(v.lines.front().big);
    CHECK(v.lines.front().color == EHudColor::Normal);
    // ...and why it went nowhere.
    CHECK(v.lines.back().text == "didn't catch a command");
    CHECK(v.lines.back().color == EHudColor::Warn);
    CHECK(v.confidence < 0.f);   // no confidence bar on a rejection
    CHECK(v.holdMs > 0);         // transient
    CHECK(v.holdMs <= 3000);     // and brief — an acknowledgement, not a read
}

TEST_CASE("hud: an empty window says nothing was heard") {
    SConfig  cfg;
    SHudView v = hudForRejection("", "", cfg);
    CHECK(v.state == EHudState::Rejected);
    REQUIRE(v.lines.size() == 1);
    CHECK(v.lines.front().text == "didn't hear anything");
    CHECK(v.lines.front().color == EHudColor::Dim);
    CHECK(v.holdMs > 0);
}

TEST_CASE("hud: a rejection note overrides the default reason line") {
    SConfig cfg;
    SHudView withText = hudForRejection("workspace three", "no speech model loaded", cfg);
    REQUIRE(withText.lines.size() == 2);
    CHECK(withText.lines.back().text == "no speech model loaded");

    SHudView noText = hudForRejection("", "no speech model loaded", cfg);
    REQUIRE(noText.lines.size() == 2);
    CHECK(noText.lines.front().text == "didn't hear anything");
    CHECK(noText.lines.back().text == "no speech model loaded");
}

TEST_CASE("hud props: a rejection outranks listening but not a clarify/error veto") {
    CHECK(hudUrgencyForState(EHudState::Rejected) > hudUrgencyForState(EHudState::Listening));
    CHECK(hudUrgencyForState(EHudState::Rejected) < hudUrgencyForState(EHudState::Clarify));
    CHECK(hudUrgencyForState(EHudState::Rejected) < hudUrgencyForState(EHudState::Error));

    SConfig   cfg;
    SHudProps p = hudPropsFromView(hudForRejection("browser", "", cfg), "voice");
    CHECK(p.slot == "voice");
    CHECK(p.kind == "text");
    CHECK(p.holdMs > 0); // transient, unlike the persistent listening panel
    CHECK(propsHasLine(p, "browser"));
    CHECK(propsHasLine(p, "didn't catch a command"));
    CHECK(p.confidence < 0.f); // no confidence prop emitted
}

TEST_CASE("hud: every state has a name") {
    CHECK(std::string(hudStateName(EHudState::Rejected)) == "rejected");
    CHECK(std::string(hudStateName(EHudState::Hidden)) == "hidden");
}

// ---- view -> hypxrhud panel-props mapping ---------------------------------------------

TEST_CASE("hud props: colour roles match hypxrhud's EColor order") {
    // Locked to hypxrhud EColor: 0 Normal, 1 Dim, 2 Accent, 3 Good, 4 Warn, 5 Bad.
    CHECK(hudColorRole(EHudColor::Normal) == 0u);
    CHECK(hudColorRole(EHudColor::Dim)    == 1u);
    CHECK(hudColorRole(EHudColor::Accent) == 2u);
    CHECK(hudColorRole(EHudColor::Good)   == 3u);
    CHECK(hudColorRole(EHudColor::Warn)   == 4u);
    CHECK(hudColorRole(EHudColor::Bad)    == 5u);
}

TEST_CASE("hud props: urgency escalates for clarify/error over routine panels") {
    CHECK(hudUrgencyForState(EHudState::Listening) == 1u);
    CHECK(hudUrgencyForState(EHudState::Action)    == 2u);
    CHECK(hudUrgencyForState(EHudState::Clarify)   > hudUrgencyForState(EHudState::Action));
    CHECK(hudUrgencyForState(EHudState::Error)     > hudUrgencyForState(EHudState::Action));
}

TEST_CASE("hud props: listening view maps to a persistent voice text panel") {
    SConfig cfg;
    SHudView v = hudForListening("open the browser", cfg);
    SHudProps p = hudPropsFromView(v, "voice");

    CHECK(p.slot == "voice");
    CHECK(p.kind == "text");
    CHECK(p.urgency == 1u);
    CHECK(p.holdMs < 0);                 // persistent — hold_ms<0 keeps it up
    CHECK(propsHasLine(p, "open the browser"));
    // The title line survives with its big flag + accent role.
    REQUIRE(!p.lines.empty());
    CHECK(p.lines.front().big);
    CHECK(hudColorRole(p.lines.front().color) == 2u); // accent
}

TEST_CASE("hud props: action view maps confidence + transient envelope + urgency") {
    SConfig cfg;
    SAction a; a.verb = EVerb::Anchor; a.target = "XR-code"; a.confidence = 0.82;
    a.targetSource = ETargetSource::Named;
    SExecPlan plan; plan.ok = true;
    SHudView v = hudForAction(a, plan, cfg);
    SHudProps p = hudPropsFromView(v, "voice");

    CHECK(p.urgency == 2u);
    CHECK(p.confidence == doctest::Approx(0.82f)); // >=0 => a confidence prop is emitted
    CHECK(p.holdMs == 2600);                       // transient dwell forwarded
    CHECK(p.fadeMs == 450);
    CHECK(propsHasLine(p, "anchoring XR-code"));
}

TEST_CASE("hud props: the configured slot override rides through to the props") {
    SConfig cfg;
    SHudView v = hudForListening("", cfg);
    SHudProps p = hudPropsFromView(v, "keys");
    CHECK(p.slot == "keys");
    // Empty slot falls back to the voice default (never an empty slot string).
    CHECK(hudPropsFromView(v, "").slot == "voice");
}
