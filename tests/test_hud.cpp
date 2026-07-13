#include "doctest.h"

#include "HudMessage.hpp"
#include "HudModel.hpp"
#include "HudText.hpp"

#include <string>

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
}

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

    SAction c; c.verb = EVerb::Clarify; c.clarifyQuestion = "which firefox?";
    CHECK(hudActionPhrase(c) == "which firefox?");
}

TEST_CASE("hud: listening panel persists (no auto-fade) and shows partial text") {
    SConfig cfg;
    SHudView v = hudForListening("open the browser", cfg);
    CHECK(v.state == EHudState::Listening);
    CHECK(v.holdMs < 0); // persistent until replaced
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
    SConfig cfg;
    SAction a; a.verb = EVerb::None;
    SExecPlan plan;
    SHudView v = hudForAction(a, plan, cfg);
    CHECK(v.state == EHudState::Hidden);
}

TEST_CASE("hud: fade envelope rises, holds, and falls to zero") {
    SHudView v;
    v.state = EHudState::Action;
    v.riseMs = 100; v.holdMs = 1000; v.fadeMs = 200; v.opacityCeil = 0.9f;

    CHECK(hudOpacity(v, 0) == doctest::Approx(0.f));
    CHECK(hudOpacity(v, 50) == doctest::Approx(0.45f));       // mid-rise
    CHECK(hudOpacity(v, 100) == doctest::Approx(0.9f));       // top of rise
    CHECK(hudOpacity(v, 600) == doctest::Approx(0.9f));       // hold plateau
    CHECK(hudOpacity(v, 1100) == doctest::Approx(0.9f));      // end of hold
    CHECK(hudOpacity(v, 1200) == doctest::Approx(0.45f));     // mid-fade
    CHECK(hudOpacity(v, 1300) == doctest::Approx(0.f));       // fully faded
    CHECK(hudOpacity(v, 99999) == doctest::Approx(0.f));
}

TEST_CASE("hud: listening panel never auto-fades") {
    SHudView v;
    v.state = EHudState::Listening;
    v.riseMs = 100; v.holdMs = -1; v.opacityCeil = 0.9f;
    CHECK(hudOpacity(v, 100) == doctest::Approx(0.9f));
    CHECK(hudOpacity(v, 100000) == doctest::Approx(0.9f)); // stays until replaced
}

TEST_CASE("hud: hidden view is fully transparent") {
    SHudView v; v.state = EHudState::Hidden;
    CHECK(hudOpacity(v, 50) == doctest::Approx(0.f));
}

TEST_CASE("hud message: round-trips a full view exactly") {
    SHudView v;
    v.state = EHudState::Action;
    v.lines = {{"anchoring XR-code", EHudColor::Accent, true},
               {"dry-run", EHudColor::Dim, false},
               {"say “hey hypr”", EHudColor::Normal, false}}; // non-ASCII survives
    v.confidence = 0.73f; v.approximated = true; v.dryRun = true;
    v.riseMs = 111; v.holdMs = 2222; v.fadeMs = 333; v.opacityCeil = 0.88f;

    std::string wire = HudMsg::serialize(v);
    CHECK(wire.back() == '\n');

    SHudView r;
    REQUIRE(HudMsg::parse(wire, r));
    CHECK(r.state == v.state);
    REQUIRE(r.lines.size() == v.lines.size());
    for (size_t i = 0; i < v.lines.size(); i++) {
        CHECK(r.lines[i].text == v.lines[i].text);
        CHECK(r.lines[i].color == v.lines[i].color);
        CHECK(r.lines[i].big == v.lines[i].big);
    }
    CHECK(r.confidence == doctest::Approx(v.confidence));
    CHECK(r.approximated == v.approximated);
    CHECK(r.dryRun == v.dryRun);
    CHECK(r.riseMs == v.riseMs);
    CHECK(r.holdMs == v.holdMs);
    CHECK(r.fadeMs == v.fadeMs);
    CHECK(r.opacityCeil == doctest::Approx(v.opacityCeil));
}

TEST_CASE("hud message: malformed input is rejected, not crashed") {
    SHudView r;
    CHECK_FALSE(HudMsg::parse("not json", r));
    CHECK_FALSE(HudMsg::parse("[1,2,3]", r)); // array, not object
    CHECK(HudMsg::parse("{}", r));            // empty object => defaults, still valid
    CHECK(r.state == EHudState::Hidden);
}

TEST_CASE("hud render: hidden view produces a fully transparent image of the right size") {
    SHudView v; v.state = EHudState::Hidden;
    SHudImage img = renderHud(v, 256, 128);
    CHECK(img.w == 256);
    CHECK(img.h == 128);
    REQUIRE(img.rgba.size() == 256u * 128u * 4u);
    uint32_t alphaSum = 0;
    for (size_t i = 3; i < img.rgba.size(); i += 4)
        alphaSum += img.rgba[i];
    CHECK(alphaSum == 0u);
}

TEST_CASE("hud render: an action panel draws visible (non-transparent) pixels") {
    SConfig cfg;
    SAction a; a.verb = EVerb::Anchor; a.target = "XR-code"; a.confidence = 0.8;
    SExecPlan plan; plan.ok = true;
    SHudView v = hudForAction(a, plan, cfg);

    SHudImage img = renderHud(v, 768, 384);
    REQUIRE(!img.empty());
    uint32_t opaqueish = 0;
    for (size_t i = 3; i < img.rgba.size(); i += 4)
        if (img.rgba[i] > 40)
            opaqueish++;
    CHECK(opaqueish > 500u); // the panel + glyphs cover a meaningful area
}
