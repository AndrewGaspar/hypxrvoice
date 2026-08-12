#include "doctest.h"

#include "Intent.hpp"

#include <string>
#include <vector>

namespace {
    // Build a transcript with per-word timestamps spaced 200 ms apart from `t0`.
    STranscript mk(const std::string& text, int64_t t0 = 100000) {
        STranscript t;
        t.text    = text;
        t.onsetMs = t0;
        std::string cur;
        int64_t     ms = t0;
        auto        flush = [&]() {
            if (cur.empty()) return;
            SWord w; w.text = cur; w.startMs = ms; w.endMs = ms + 150;
            t.words.push_back(w);
            ms += 200; cur.clear();
        };
        for (char c : text) { if (c == ' ') flush(); else cur += c; }
        flush();
        t.endMs = ms;
        return t;
    }

    SDesktopContext fixture() {
        const char* mons = R"json([
            {"id":0,"name":"eDP-1","focused":true},
            {"id":3,"name":"XR-code"},
            {"id":4,"name":"XR-web"}
        ])json";
        const char* cls = R"json([
            {"class":"nvim","title":"main.cpp - NVIM","monitor":3,"mapped":true},
            {"class":"firefox","title":"YouTube - Mozilla Firefox","monitor":4,"mapped":true}
        ])json";
        const char* xr = R"json({"state":"focused","monitors":[
            {"name":"XR-code","id":3,"anchor":{"mode":"local"}},
            {"name":"XR-web","id":4,"hovered":true,"anchor":{"mode":"body"}}
        ]})json";
        return SDesktopContext::parse(mons, cls, xr);
    }

    SIntentConfig icfg() { SIntentConfig c; c.distanceStep = 0.25; return c; }
    GazeQueryFn   noGaze = [](int64_t) { return std::string(); };
}

TEST_CASE("intent: keyword grammar detects verbs and deixis") {
    CRuleIntent eng(icfg());

    SRawIntent a = eng.detect(mk("move the coding monitor closer"));
    CHECK(a.verb == EVerb::MoveDist);
    CHECK(a.deltaM < 0);
    CHECK(a.targetPhrase.find("coding") != std::string::npos);
    CHECK_FALSE(a.deictic);

    SRawIntent b = eng.detect(mk("pick this monitor up"));
    CHECK(b.verb == EVerb::Pick);
    CHECK(b.deictic);
    CHECK_FALSE(b.deicticIsPlace);

    SRawIntent c = eng.detect(mk("place it here"));
    CHECK(c.verb == EVerb::Place);
    CHECK(c.deictic);
    CHECK(c.deicticIsPlace);

    SRawIntent d = eng.detect(mk("have youtube follow me"));
    CHECK(d.verb == EVerb::Follow);
    CHECK(d.targetPhrase.find("youtube") != std::string::npos);

    SRawIntent e = eng.detect(mk("push it further away"));
    CHECK(e.verb == EVerb::MoveDist);
    CHECK(e.deltaM > 0);

    SRawIntent f = eng.detect(mk("turn my hands off"));
    CHECK(f.verb == EVerb::HandInput);
    CHECK(f.sub == "off");

    SRawIntent viewOff = eng.detect(mk("hide monitor view"));
    CHECK(viewOff.verb == EVerb::MonitorView);
    CHECK(viewOff.sub == "off");
    SRawIntent viewOn = eng.detect(mk("show monitor view"));
    CHECK(viewOn.verb == EVerb::MonitorView);
    CHECK(viewOn.sub == "on");
    SRawIntent viewToggle = eng.detect(mk("toggle monitor view"));
    CHECK(viewToggle.verb == EVerb::MonitorView);
    CHECK(viewToggle.sub == "toggle");
    SRawIntent monitorsOff = eng.detect(mk("hide monitors"));
    CHECK(monitorsOff.verb == EVerb::MonitorView);
    CHECK(monitorsOff.sub == "off");
    SRawIntent monitorsOn = eng.detect(mk("show monitors"));
    CHECK(monitorsOn.verb == EVerb::MonitorView);
    CHECK(monitorsOn.sub == "on");
    SRawIntent monitorsToggle = eng.detect(mk("toggle monitors"));
    CHECK(monitorsToggle.verb == EVerb::MonitorView);
    CHECK(monitorsToggle.sub == "toggle");

    // Broad show/hide speech must not actuate this global switch.
    CHECK(eng.detect(mk("hide the browser")).verb == EVerb::None);
    CHECK(eng.detect(mk("show my desktop")).verb == EVerb::None);
    CHECK(eng.detect(mk("hide monitor viewpoint")).verb == EVerb::None);
    CHECK(eng.detect(mk("don't hide monitor view")).verb == EVerb::None);
    CHECK(eng.detect(mk("don't hide monitors")).verb == EVerb::None);
    CHECK(eng.detect(mk("show and hide monitor view")).verb == EVerb::None);
    CHECK(eng.detect(mk("show and hide monitors")).verb == EVerb::None);

    // Out of scope: no command keyword.
    SRawIntent g = eng.detect(mk("what is the weather today"));
    CHECK(g.verb == EVerb::None);
}

TEST_CASE("intent: semantic target resolves against the live snapshot") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("move the coding monitor closer"), fixture(), noGaze);
    CHECK(a.verb == EVerb::MoveDist);
    CHECK(a.target == "XR-code");
    CHECK(a.targetSource == ETargetSource::Semantic);
    CHECK(a.deltaM < 0);
}

TEST_CASE("intent: 'have youtube follow me' resolves the browser monitor") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("have youtube follow me"), fixture(), noGaze);
    CHECK(a.verb == EVerb::Follow);
    CHECK(a.target == "XR-web");
}

TEST_CASE("intent: deictic 'this' picks the gazed monitor at word time") {
    CRuleIntent eng(icfg());
    // Gaze ring says the user is looking at XR-web.
    GazeQueryFn q = [](int64_t at) {
        char b[300];
        std::snprintf(b, sizeof(b),
                      R"json({"ok":true,"viewValid":true,"timestampMs":%lld,
                      "head":{"pos":[0,1.4,0],"forward":[0,0,-1]},
                      "gaze":{"monitorId":4,"name":"XR-web","selected":true,"dwellSec":0.3},
                      "query":{"matchedTimestampMs":%lld,"ageMs":0}})json",
                      (long long)at, (long long)at);
        return std::string(b);
    };
    SAction a = eng.resolve(mk("pick this monitor up"), fixture(), q);
    CHECK(a.verb == EVerb::Pick);
    CHECK(a.target == "XR-web");
    CHECK(a.targetSource == ETargetSource::Deixis);
    CHECK(a.gaze.valid);
}

TEST_CASE("intent: 'place it here' keeps the active target and attaches the gaze pose") {
    CRuleIntent eng(icfg());
    GazeQueryFn q = [](int64_t at) {
        char b[320];
        std::snprintf(b, sizeof(b),
                      R"json({"ok":true,"viewValid":true,"timestampMs":%lld,
                      "head":{"pos":[0.5,1.4,-1.2],"forward":[0,0,-1]},
                      "gaze":{"monitorId":-1,"name":"","selected":false,"dwellSec":0},
                      "query":{"matchedTimestampMs":%lld,"ageMs":0}})json",
                      (long long)at, (long long)at);
        return std::string(b);
    };
    SAction a = eng.resolve(mk("place it here"), fixture(), q);
    CHECK(a.verb == EVerb::Place);
    CHECK(a.target == "active");
    CHECK(a.targetSource == ETargetSource::Deixis);
    CHECK(a.gaze.valid);
    CHECK(a.gaze.pos[0] == doctest::Approx(0.5));
}

TEST_CASE("intent: out-of-scope utterance yields None (no command)") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("i think it might rain later"), fixture(), noGaze);
    CHECK(a.verb == EVerb::None);
    CHECK_FALSE(a.actionable());
}

TEST_CASE("intent: ambiguous target keeps a best guess but marks low confidence") {
    const char* mons = R"json([{"id":3,"name":"XR-a"},{"id":4,"name":"XR-b"}])json";
    const char* cls  = R"json([
        {"class":"firefox","title":"Docs","monitor":3,"mapped":true},
        {"class":"firefox","title":"Mail","monitor":4,"mapped":true}
    ])json";
    SDesktopContext ctx = SDesktopContext::parse(mons, cls, "");

    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("move firefox closer"), ctx, noGaze);
    CHECK(a.verb == EVerb::MoveDist);
    CHECK(a.confidence < 0.5);
    CHECK(a.clarifyCandidates.size() == 2);
    // A best guess is still emitted (feedback shows the alternatives).
    CHECK((a.target == "XR-a" || a.target == "XR-b"));
}

TEST_CASE("intent: sanitizeDeltaM — utterance direction is authoritative, magnitude is the step") {
    // The live bug: model said +100 for "closer" — direction flipped, magnitude reset.
    CHECK(sanitizeDeltaM(100.0, "move the coding monitor closer", 0.25) == doctest::Approx(-0.25));
    // Round 6: a BARE direction word names no amount, so the configured step is the answer
    // whatever number the backend attached. A 3B run answered bare "move closer" with
    // -1.00 m — inside the old [0.05, 1.0] clamp, and it landed the monitor on the wearer.
    CHECK(sanitizeDeltaM(-1.0, "move closer", 0.25) == doctest::Approx(-0.25));
    CHECK(sanitizeDeltaM(-0.1, "bring it closer", 0.25) == doctest::Approx(-0.25));
    CHECK(sanitizeDeltaM(0.9, "move further away", 0.25) == doctest::Approx(0.25));
    CHECK(sanitizeDeltaM(0.1, "push it further away", 0.25) == doctest::Approx(0.25));
    // Model sign disagrees with the utterance: utterance wins.
    CHECK(sanitizeDeltaM(0.3, "a bit closer please", 0.25) == doctest::Approx(-0.25));
    // An amount the user actually SPOKE keeps the backend's number (still clamped). The
    // unit word is the evidence that a quantity was asked for.
    CHECK(sanitizeDeltaM(-0.5, "move it half a meter closer", 0.25) == doctest::Approx(-0.5));
    CHECK(sanitizeDeltaM(0.75, "push it 75 centimeters away", 0.25) == doctest::Approx(0.75));
    CHECK(sanitizeDeltaM(-9.0, "move it two meters closer", 0.25) == doctest::Approx(-0.25)); // absurd -> step
    // An incidental number is not a spoken distance.
    CHECK(sanitizeDeltaM(-1.0, "move monitor 2 closer", 0.25) == doctest::Approx(-0.25));
    // No direction word: model sign trusted, magnitude is the step.
    CHECK(sanitizeDeltaM(-50.0, "adjust the distance", 0.25) == doctest::Approx(-0.25));
    // Zero/absent value: step magnitude, default pull.
    CHECK(sanitizeDeltaM(0.0, "adjust it", 0.25) == doctest::Approx(-0.25));
    // A different configured step is honoured verbatim.
    CHECK(sanitizeDeltaM(-1.0, "move closer", 0.4) == doctest::Approx(-0.4));
    // Idempotent: finalize re-runs it over an action a backend already sanitized.
    CHECK(sanitizeDeltaM(sanitizeDeltaM(-1.0, "move closer", 0.25), "move closer", 0.25) ==
          doctest::Approx(-0.25));
}

TEST_CASE("intent: finalize pins the push/pull magnitude for EVERY backend") {
    // A llama-style raw intent: the verb and a direction the model got right, and a
    // magnitude it invented. finalizeAction is backend-agnostic, so the guard holds even
    // when the raw intent never went through the rule grammar.
    SRawIntent raw;
    raw.verb   = EVerb::MoveDist;
    raw.deltaM = -1.0;
    raw.note   = "llama";
    SAction a = finalizeAction(raw, mk("move closer"), fixture(), noGaze, icfg());
    CHECK(a.verb == EVerb::MoveDist);
    CHECK(a.deltaM == doctest::Approx(-0.25));

    // …and a spoken amount still reaches the executor intact.
    SRawIntent spoken;
    spoken.verb   = EVerb::MoveDist;
    spoken.deltaM = -0.5;
    SAction b = finalizeAction(spoken, mk("move it half a meter closer"), fixture(), noGaze, icfg());
    CHECK(b.deltaM == doctest::Approx(-0.5));
}

// ---- window management (round 2) ---------------------------------------------------
//
// Every phrasing below is one the user actually spoke into the headset during live
// round 2 and got "intent none" for. They are pinned here verbatim (including the two
// forms whisper alternates between for a spoken number) so the grammar cannot quietly
// regress on them again.

namespace {
    // A snapshot with real clients: addresses, classes and titles, so the window verbs
    // have something live to resolve against.
    SDesktopContext windowFixture() {
        const char* mons = R"json([
            {"id":0,"name":"eDP-1","focused":true},
            {"id":3,"name":"XR-code"},
            {"id":4,"name":"XR-web"}
        ])json";
        const char* cls = R"json([
            {"address":"0x1111","class":"firefox","title":"YouTube - Mozilla Firefox",
             "monitor":4,"mapped":true,"focusHistoryID":1},
            {"address":"0x2222","class":"nvim","title":"main.cpp - NVIM",
             "monitor":3,"mapped":true,"focusHistoryID":0},
            {"address":"0x3333","class":"kitty","title":"~/code",
             "monitor":0,"mapped":true,"focusHistoryID":2}
        ])json";
        const char* xr = R"json({"state":"focused","monitors":[
            {"name":"XR-code","id":3,"anchor":{"mode":"local"}},
            {"name":"XR-web","id":4,"anchor":{"mode":"body"}}
        ]})json";
        return SDesktopContext::parse(mons, cls, xr);
    }
}

TEST_CASE("intent: every spoken fullscreen phrasing parses") {
    CRuleIntent eng(icfg());
    for (const char* phrase : {"make this window fullscreen", "make the window fullscreen",
                               "fullscreen this", "make it fullscreen", "fullscreen",
                               "make this window full screen"}) {
        INFO("phrase: ", phrase);
        CHECK(eng.detect(mk(phrase)).verb == EVerb::Fullscreen);
    }
    // "maximize" is Hyprland's other fullscreen mode, not a separate verb.
    SRawIntent m = eng.detect(mk("maximize this window"));
    CHECK(m.verb == EVerb::Fullscreen);
    CHECK(m.sub == "maximize");
}

TEST_CASE("intent: 'fullscreen the browser' resolves the live browser window") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("fullscreen the browser"), windowFixture(), noGaze);
    CHECK(a.verb == EVerb::Fullscreen);
    CHECK(a.windowAddress == "0x1111");
    CHECK(a.windowLabel == "firefox");
}

TEST_CASE("intent: a bare fullscreen acts on the focused window, not a guessed one") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("make this window fullscreen"), windowFixture(), noGaze);
    CHECK(a.verb == EVerb::Fullscreen);
    CHECK(a.windowAddress.empty());
    CHECK(a.target == "active");
}

TEST_CASE("intent: 'workspace three' parses as digits AND as a number word") {
    CRuleIntent eng(icfg());
    // Whisper alternated between these two forms for the SAME spoken utterance during
    // live round 2 ("3." on one attempt, "three." on the next).
    struct { const char* text; int want; } cases[] = {
        {"workspace three", 3},          {"workspace 3", 3},
        {"go to workspace three", 3},    {"go to workspace 3", 3},
        {"switch to workspace seven", 7},{"switch to workspace 7", 7},
        {"workspace one", 1},            {"workspace ten", 10},
        {"workspace 12", 12},            {"work space four", 4},
    };
    for (auto& c : cases) {
        INFO("phrase: ", c.text);
        SRawIntent r = eng.detect(mk(c.text));
        CHECK(r.verb == EVerb::Workspace);
        CHECK(r.workspace == c.want);
    }
}

TEST_CASE("intent: 'workspace' with no number asks rather than guessing") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("go to workspace"), windowFixture(), noGaze);
    CHECK(a.verb == EVerb::Clarify);
    CHECK(a.workspace == 0);
}

TEST_CASE("intent: 'focus the browser' resolves content-first to the live window") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("focus the browser"), windowFixture(), noGaze);
    CHECK(a.verb == EVerb::Focus);
    CHECK(a.windowAddress == "0x1111");
    CHECK(a.windowLabel == "firefox");
    CHECK(a.confidence > 0.6);

    SAction b = eng.resolve(mk("focus the editor"), windowFixture(), noGaze);
    CHECK(b.verb == EVerb::Focus);
    CHECK(b.windowLabel == "nvim");

    SAction c = eng.resolve(mk("switch to the terminal"), windowFixture(), noGaze);
    CHECK(c.verb == EVerb::Focus);
    CHECK(c.windowLabel == "kitty");

    // Naming the app outright works just as well as the generic noun.
    SAction d = eng.resolve(mk("focus firefox"), windowFixture(), noGaze);
    CHECK(d.verb == EVerb::Focus);
    CHECK(d.windowAddress == "0x1111");
}

TEST_CASE("intent: a focus target we cannot see asks instead of picking something") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("focus the spreadsheet"), windowFixture(), noGaze);
    CHECK(a.verb == EVerb::Clarify);
    CHECK(a.windowAddress.empty());
}

// THE SAFETY CASE. When the leading word is lost — which is exactly what live round 2
// produced, over and over — what survives is a bare trailing fragment. Those must stay
// rejected: guessing a workspace switch or a focus change from "3." or "browser." would
// turn a transcription failure into a wrong action.
TEST_CASE("intent: a bare trailing fragment stays rejected, never guessed") {
    CRuleIntent eng(icfg());
    for (const char* frag : {"3", "three", "browser", "editor", "terminal", "window",
                             "3.", "browser.", "the browser"}) {
        INFO("fragment: ", frag);
        CHECK(eng.detect(mk(frag)).verb == EVerb::None);
    }
    // And the negative control from the live round: ordinary speech is not a command.
    CHECK(eng.detect(mk("I wonder what's for dinner")).verb == EVerb::None);
}

// ---- window -> monitor / workspace moves (round 3) ---------------------------------
//
// "Move terminal to the left monitor." transcribed PERFECTLY during live round 3 and
// still came back intent=none: the grammar had no verb for relocating a window. The
// destination half is the interesting part — a spoken "the left monitor" is a claim
// about the LAYOUT, so it resolves against monitor geometry, never against a name.

namespace {
    // Three outputs in a row, with the x coordinates `hyprctl monitors -j` reports.
    SDesktopContext spatialFixture() {
        const char* mons = R"json([
            {"id":0,"name":"eDP-1","focused":true,"x":0,"y":0,"width":1920,"height":1080},
            {"id":3,"name":"XR-code","x":1920,"y":0,"width":2560,"height":1440},
            {"id":4,"name":"XR-web","x":4480,"y":0,"width":2560,"height":1440}
        ])json";
        const char* cls = R"json([
            {"address":"0x1111","class":"firefox","title":"YouTube - Mozilla Firefox",
             "monitor":4,"mapped":true,"focusHistoryID":1},
            {"address":"0x2222","class":"nvim","title":"main.cpp - NVIM",
             "monitor":3,"mapped":true,"focusHistoryID":0},
            {"address":"0x3333","class":"kitty","title":"~",
             "monitor":0,"mapped":true,"focusHistoryID":2}
        ])json";
        return SDesktopContext::parse(mons, cls, "");
    }
}

TEST_CASE("intent: 'move terminal to the left monitor' parses into both halves") {
    CRuleIntent eng(icfg());
    SRawIntent r = eng.detect(mk("Move terminal to the left monitor."));
    CHECK(r.verb == EVerb::MoveWindow);
    CHECK(r.windowPhrase == "terminal");
    CHECK(r.spatial == ESpatialRef::Left);
    CHECK(r.monitorPhrase.empty());
}

TEST_CASE("intent: the spoken move phrasings all resolve to a live window and a live monitor") {
    CRuleIntent eng(icfg());
    struct { const char* text; const char* addr; const char* mon; } cases[] = {
        {"move terminal to the left monitor",        "0x3333", "eDP-1"},
        {"send the browser to the right monitor",    "0x1111", "XR-web"},
        {"put the editor on the left screen",        "0x2222", "eDP-1"},
        {"move the browser over to the right",       "0x1111", "XR-web"},
        {"send firefox to the leftmost display",     "0x1111", "eDP-1"},
    };
    for (auto& c : cases) {
        INFO("phrase: ", c.text);
        SAction a = eng.resolve(mk(c.text), spatialFixture(), noGaze);
        CHECK(a.verb == EVerb::MoveWindow);
        CHECK(a.windowAddress == c.addr);
        CHECK(a.target == c.mon);
        CHECK(a.workspace == 0);
    }
}

TEST_CASE("intent: a named destination monitor resolves semantically") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("move the browser to the coding monitor"), spatialFixture(), noGaze);
    CHECK(a.verb == EVerb::MoveWindow);
    CHECK(a.windowAddress == "0x1111");
    CHECK(a.target == "XR-code");
}

TEST_CASE("intent: 'move this to the left monitor' moves the FOCUSED window") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("move this window to the left monitor"), spatialFixture(), noGaze);
    CHECK(a.verb == EVerb::MoveWindow);
    CHECK(a.windowAddress.empty()); // empty == "whatever is focused"
    CHECK(a.target == "eDP-1");
}

TEST_CASE("intent: '…to this monitor' takes the destination from the gaze ring") {
    CRuleIntent eng(icfg());
    GazeQueryFn q = [](int64_t at) {
        char b[320];
        std::snprintf(b, sizeof(b),
                      R"json({"ok":true,"viewValid":true,"timestampMs":%lld,
                      "head":{"pos":[0,1.4,0],"forward":[0,0,-1]},
                      "gaze":{"monitorId":3,"name":"XR-code","selected":true,"dwellSec":0.4},
                      "query":{"matchedTimestampMs":%lld,"ageMs":0}})json",
                      (long long)at, (long long)at);
        return std::string(b);
    };
    SAction a = eng.resolve(mk("move the browser to this monitor"), spatialFixture(), q);
    CHECK(a.verb == EVerb::MoveWindow);
    CHECK(a.windowAddress == "0x1111");
    CHECK(a.target == "XR-code");
    CHECK(a.targetSource == ETargetSource::Deixis);
}

TEST_CASE("intent: a workspace destination is a MOVE, not a workspace switch") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("move the browser to workspace three"), spatialFixture(), noGaze);
    CHECK(a.verb == EVerb::MoveWindow);
    CHECK(a.windowAddress == "0x1111");
    CHECK(a.workspace == 3);
    CHECK(a.target.empty());

    // …while the bare switch keeps its own verb.
    CHECK(eng.detect(mk("go to workspace three")).verb == EVerb::Workspace);
    // "move to workspace three" names no window, so it stays a switch rather than
    // silently relocating whatever happened to be focused.
    CHECK(eng.detect(mk("move to workspace three")).verb == EVerb::Workspace);
}

TEST_CASE("intent: a move with no number for its workspace asks") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("move the browser to the workspace"), spatialFixture(), noGaze);
    CHECK(a.verb == EVerb::Clarify);
}

TEST_CASE("intent: 'the left monitor' is refused when the layout cannot answer") {
    CRuleIntent eng(icfg());
    // One monitor: nothing is "the left one".
    SDesktopContext solo = SDesktopContext::parse(
        R"json([{"id":0,"name":"eDP-1","focused":true,"x":0}])json",
        R"json([{"address":"0x3333","class":"kitty","title":"~","monitor":0,"mapped":true,"focusHistoryID":0}])json",
        "");
    SAction a = eng.resolve(mk("move the terminal to the left monitor"), solo, noGaze);
    CHECK(a.verb == EVerb::Clarify);

    // Two monitors stacked at the SAME x: "left" is genuinely ambiguous, so ask.
    SDesktopContext stacked = SDesktopContext::parse(
        R"json([{"id":0,"name":"DP-1","x":0,"y":0},{"id":1,"name":"DP-2","x":0,"y":1080}])json",
        R"json([{"address":"0x3333","class":"kitty","title":"~","monitor":0,"mapped":true,"focusHistoryID":0}])json",
        "");
    SAction b = eng.resolve(mk("move the terminal to the left monitor"), stacked, noGaze);
    CHECK(b.verb == EVerb::Clarify);
    CHECK(b.clarifyCandidates.size() == 2);
}

TEST_CASE("intent: a destination we cannot see asks rather than moving the window anywhere") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("move the browser to the gaming monitor"), spatialFixture(), noGaze);
    CHECK(a.verb == EVerb::Clarify);
    CHECK(a.target.empty());
}

TEST_CASE("intent: the move grammar declines anything that is not one") {
    CRuleIntent eng(icfg());
    // A destination must announce itself as an output or a workspace. Without that
    // "move this closer to me" would parse as a move-to-"me".
    CHECK(eng.detect(mk("move this closer to me")).verb == EVerb::MoveDist);
    CHECK(eng.detect(mk("move the coding monitor closer")).verb == EVerb::MoveDist);
    // Round 6 moved this one: "put THE EDITOR here" names a window AND a destination, so
    // it is a window move. The XR place verb is what you say about the thing you are
    // already holding — "put IT here" — and it keeps that reading (see the double-deixis
    // test at the end of this file).
    CHECK(eng.detect(mk("put the editor here")).verb == EVerb::MoveWindow);
    CHECK(eng.detect(mk("put it here")).verb == EVerb::Place);
    CHECK(eng.detect(mk("bring it closer")).verb == EVerb::MoveDist);
    CHECK(eng.detect(mk("push it further away")).verb == EVerb::MoveDist);
    // Round 6 widened the destination prepositions to in/at/onto. These are the near
    // misses that must still NOT become moves: the destination has to name an output.
    CHECK(eng.detect(mk("put it in front of me")).verb == EVerb::Center);
    CHECK(eng.detect(mk("move it in front of me")).verb == EVerb::Center);
    CHECK(eng.detect(mk("move it back")).verb == EVerb::MoveDist);
}

// ---------------------------------------------------------------------------
// Round 5, Task C: an unmatched window reference must NEVER actuate.
//
// Live fire: "Move workspace forward to this monitor." parsed as move_window with a
// deictic destination at confidence 0.90 and dispatched `movewindow mon:XR-main` on
// whatever window happened to be focused. Two things were wrong: a workspace phrase was
// read as a window reference, and an unmatched window reference silently fell through to
// the focused window. The user had actually said "move workspace 4" — Whisper renders
// "four" as "forward".
// ---------------------------------------------------------------------------

namespace {
    // A fixture whose windows include btop, so the "named window that IS live" case is
    // exercised against a real match rather than a hypothetical one.
    SDesktopContext btopFixture() {
        const char* mons = R"json([
            {"id":0,"name":"eDP-1","focused":true,"x":0,"y":0,"width":1920,"height":1080},
            {"id":3,"name":"XR-code","x":1920,"y":0,"width":1920,"height":1080},
            {"id":4,"name":"XR-web","x":3840,"y":0,"width":1920,"height":1080}
        ])json";
        const char* cls = R"json([
            {"class":"btop","title":"btop","monitor":3,"mapped":true,"address":"0xaaa","focusHistoryID":2},
            {"class":"nvim","title":"main.cpp - NVIM","monitor":3,"mapped":true,"address":"0xbbb","focusHistoryID":0},
            {"class":"firefox","title":"YouTube - Mozilla Firefox","monitor":4,"mapped":true,"address":"0xccc","focusHistoryID":1}
        ])json";
        const char* xr = R"json({"state":"focused","monitors":[
            {"name":"XR-code","id":3,"anchor":{"mode":"local"}},
            {"name":"XR-web","id":4,"hovered":true,"anchor":{"mode":"body"}}
        ]})json";
        return SDesktopContext::parse(mons, cls, xr);
    }

    GazeQueryFn gazeAt(int id, const char* name) {
        return [=](int64_t at) {
            char b[360];
            std::snprintf(b, sizeof(b),
                          R"json({"ok":true,"viewValid":true,"timestampMs":%lld,
                          "head":{"pos":[0,1.4,0],"forward":[0,0,-1]},
                          "gaze":{"monitorId":%d,"name":"%s","selected":%s,"dwellSec":0.3},
                          "query":{"matchedTimestampMs":%lld,"ageMs":0}})json",
                          (long long)at, id, id >= 0 ? name : "", id >= 0 ? "true" : "false",
                          (long long)at);
            return std::string(b);
        };
    }
}

TEST_CASE("intent: a workspace phrase is never a window reference") {
    CRuleIntent eng(icfg());
    SRawIntent r = eng.detect(mk("move workspace forward to this monitor"));
    CHECK(r.verb == EVerb::MoveWorkspace);
    CHECK(r.windowPhrase.empty());   // the dangerous fall-through, gone
}

TEST_CASE("intent: 'move workspace forward to this monitor' is workspace 4, not a window move") {
    // The exact live-fire utterance. "forward" is Whisper's rendering of "four".
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("move workspace forward to this monitor"), btopFixture(),
                            gazeAt(3, "XR-code"));
    CHECK(a.verb == EVerb::MoveWorkspace);
    CHECK(a.workspace == 4);
    CHECK(a.target == "XR-code");
    CHECK(a.targetSource == ETargetSource::Deixis);
    CHECK(a.windowAddress.empty()); // no window is involved in this verb at all
}

TEST_CASE("intent: workspace number-slot homophones") {
    CRuleIntent eng(icfg());
    struct { const char* said; int want; } cases[] = {
        {"move workspace four to the left monitor",    4},
        {"move workspace for to the left monitor",     4},
        {"move workspace forward to the left monitor", 4},
        {"move workspace 4 to the left monitor",       4},
        {"move workspace too to the left monitor",     2},
        {"move workspace ate to the left monitor",     8},
        {"move workspace won to the left monitor",     1},
        {"move workspace tree to the left monitor",    3},
    };
    for (auto& c : cases) {
        SAction a = eng.resolve(mk(c.said), btopFixture(), noGaze);
        CHECK(a.verb == EVerb::MoveWorkspace);
        CHECK(a.workspace == c.want);
        CHECK(a.target == "eDP-1"); // leftmost by layout x
    }
}

TEST_CASE("intent: the homophone table stays in the number slot") {
    // "for"/"to" outside a workspace number slot must remain ordinary English — the
    // guard against this becoming general fuzzy matching.
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("move firefox to the left monitor"), btopFixture(), noGaze);
    CHECK(a.verb == EVerb::MoveWindow);
    CHECK(a.workspace == 0);
}

TEST_CASE("intent: an unmatched window phrase clarifies instead of moving the focused window") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("move the spreadsheet to the left monitor"), btopFixture(), noGaze);
    CHECK(a.verb == EVerb::Clarify);
    CHECK(a.windowAddress.empty());
    CHECK_FALSE(a.actionable());
}

TEST_CASE("intent: an explicit deixis still means the focused window") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("move this to the left monitor"), btopFixture(), noGaze);
    CHECK(a.verb == EVerb::MoveWindow);
    CHECK(a.windowAddress.empty()); // "the one I am using" — the executor's active-window path
    CHECK(a.target == "eDP-1");

    SAction b = eng.resolve(mk("move this window to the left monitor"), btopFixture(), noGaze);
    CHECK(b.verb == EVerb::MoveWindow);
    CHECK(b.windowAddress.empty());
}

TEST_CASE("intent: a named window that IS live still resolves semantically") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("move btop to the left monitor"), btopFixture(), noGaze);
    CHECK(a.verb == EVerb::MoveWindow);
    CHECK(a.windowAddress == "0xaaa");
    CHECK(a.target == "eDP-1");
}

TEST_CASE("intent: 'move the terminal to workspace three' keeps its window-move reading") {
    // The workspace token is in the DESTINATION half here, not the subject.
    CRuleIntent eng(icfg());
    SRawIntent r = eng.detect(mk("move the terminal to workspace three"));
    CHECK(r.verb == EVerb::MoveWindow);
    CHECK(r.sub == "workspace");
    CHECK(r.workspace == 3);
}

TEST_CASE("intent: a workspace move with no usable index clarifies") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("move workspace to the left monitor"), btopFixture(), noGaze);
    CHECK(a.verb == EVerb::Clarify);
}

// ---------------------------------------------------------------------------
// Round 5, Task B: "create a monitor here".
// ---------------------------------------------------------------------------

TEST_CASE("intent: 'create a monitor here' mints a free name and carries the gaze point") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("create a monitor here"), btopFixture(), gazeAt(-1, ""));
    CHECK(a.verb == EVerb::CreateMonitor);
    CHECK(a.target == "XR-2");     // lowest free XR-<n>, nothing live collides
    CHECK(a.gaze.valid);
    CHECK(a.gaze.placeDistM > 0.0);
    CHECK(a.gaze.place[2] == doctest::Approx(-1.3)); // 1.3 m ahead of the head at z=0
}

TEST_CASE("intent: create-monitor phrasings, and the ones that must NOT match") {
    CRuleIntent eng(icfg());
    for (const char* said : {"create a monitor here", "add a new monitor",
                             "make a new screen here", "create a display"}) {
        CHECK(eng.detect(mk(said)).verb == EVerb::CreateMonitor);
    }
    // A monitor noun alone, or a creation verb alone, is not a create.
    CHECK(eng.detect(mk("make this window fullscreen")).verb == EVerb::Fullscreen);
    CHECK(eng.detect(mk("move the coding monitor closer")).verb == EVerb::MoveDist);
    CHECK(eng.detect(mk("open the browser")).verb == EVerb::LaunchApp);
}

TEST_CASE("intent: create-monitor skips names already in use") {
    const char* mons = R"json([
        {"id":0,"name":"eDP-1","focused":true},
        {"id":3,"name":"XR-2"},
        {"id":4,"name":"XR-3"}
    ])json";
    SDesktopContext ctx = SDesktopContext::parse(mons, "[]", "");
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("create a monitor"), ctx, noGaze);
    CHECK(a.verb == EVerb::CreateMonitor);
    CHECK(a.target == "XR-4");
}

TEST_CASE("intent: create-monitor needs the noun to FOLLOW the creation verb") {
    CRuleIntent eng(icfg());
    // The dangerous near-misses: both words present, no creation meant.
    CHECK(eng.detect(mk("make this monitor follow me")).verb == EVerb::Follow);
    CHECK(eng.detect(mk("make this monitor world locked")).verb == EVerb::Anchor);
    CHECK(eng.detect(mk("make the coding monitor closer")).verb == EVerb::MoveDist);
    CHECK(eng.detect(mk("dock this monitor here")).verb == EVerb::Dock);
    // And the real thing, in the phrasings people actually use.
    CHECK(eng.detect(mk("create another monitor")).verb == EVerb::CreateMonitor);
    CHECK(eng.detect(mk("make me a new monitor here")).verb == EVerb::CreateMonitor);
    CHECK(eng.detect(mk("open a new monitor")).verb == EVerb::CreateMonitor);
}

// ---------------------------------------------------------------------------
// Round 6, Task A: a bare trailing "here" is a MONITOR destination.
//
// Live fire, with a Plex window up on workspace 2:
//   "Move WhatsApp to this monitor."  -> worked (the destination said "monitor")
//   "Move Plex here." / "Move Batman here."  -> intent NONE
//   "Move workspace two here." (x3)   -> degraded to the workspace SWITCH verb
//   "Remove workspace 4 here."        -> ditto ('move' misheard as 'remove')
// The window resolver was never at fault — it already reads class AND title tokens. The
// destination grammar required "to <monitor-ref>", so a bare trailing "here" failed the
// parse before any window was looked up.
// ---------------------------------------------------------------------------

namespace {
    // The live shape of that evening: a laptop panel, two XR monitors, and a Plex window
    // whose TITLE carries the media name (which is all "Move Batman here" has to go on).
    SDesktopContext plexFixture() {
        const char* mons = R"json([
            {"id":0,"name":"eDP-1","focused":true,"x":0,"y":0,"width":1920,"height":1080},
            {"id":3,"name":"XR-code","x":1920,"y":0,"width":1920,"height":1080},
            {"id":4,"name":"XR-web","x":3840,"y":0,"width":1920,"height":1080}
        ])json";
        const char* cls = R"json([
            {"class":"Plex","title":"Batman Begins - Plex","monitor":3,"mapped":true,
             "address":"0xp1ex","focusHistoryID":2},
            {"class":"nvim","title":"main.cpp - NVIM","monitor":3,"mapped":true,
             "address":"0xbbb","focusHistoryID":0},
            {"class":"firefox","title":"YouTube - Mozilla Firefox","monitor":4,"mapped":true,
             "address":"0xccc","focusHistoryID":1}
        ])json";
        const char* xr = R"json({"state":"focused","monitors":[
            {"name":"XR-code","id":3,"anchor":{"mode":"local"}},
            {"name":"XR-web","id":4,"anchor":{"mode":"body"}}
        ]})json";
        return SDesktopContext::parse(mons, cls, xr);
    }
}

TEST_CASE("intent: 'move Plex here' moves the window to the monitor under gaze") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("move Plex here"), plexFixture(), gazeAt(4, "XR-web"));
    CHECK(a.verb == EVerb::MoveWindow);
    CHECK(a.windowAddress == "0xp1ex");
    CHECK(a.target == "XR-web");
    CHECK(a.targetSource == ETargetSource::Deixis);
    // "here" names the MONITOR, not the projected place point — a window lands on an
    // output, and nothing in this plan may read a 3D pose.
    CHECK(a.workspace == 0);
}

TEST_CASE("intent: 'move Batman here' reaches the window through its live TITLE") {
    // The other half of the live pair: "Batman" is nowhere in a window class, only in the
    // Plex window's media title. The resolver has always scored title tokens; this pins
    // that the new destination grammar actually gets there.
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("move Batman here"), plexFixture(), gazeAt(3, "XR-code"));
    CHECK(a.verb == EVerb::MoveWindow);
    CHECK(a.windowAddress == "0xp1ex");
    CHECK(a.target == "XR-code");
}

TEST_CASE("intent: the bare-'here' destination in the phrasings people speak") {
    CRuleIntent eng(icfg());
    for (const char* said : {"move Plex here", "put Plex here", "send Plex here",
                             "move Plex over here", "move Plex right here",
                             "move Plex over there", "put Plex here please"}) {
        SAction a = eng.resolve(mk(said), plexFixture(), gazeAt(4, "XR-web"));
        CHECK_MESSAGE(a.verb == EVerb::MoveWindow, said);
        CHECK_MESSAGE(a.windowAddress == "0xp1ex", said);
        CHECK_MESSAGE(a.target == "XR-web", said);
    }
}

TEST_CASE("intent: 'here' with nothing under gaze asks instead of guessing a monitor") {
    // A gaze that landed on passthrough resolves no monitor. The one thing this must not
    // do is fall back to the active/focused output — that is precisely where the user was
    // NOT looking.
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("move Plex here"), plexFixture(), gazeAt(-1, ""));
    CHECK(a.verb == EVerb::Clarify);
    CHECK(a.clarifyQuestion == "look at the target monitor");
    CHECK(a.target.empty());
    CHECK_FALSE(a.actionable());

    // No gaze data at all (no compositor / no ring) is the same answer.
    SAction b = eng.resolve(mk("move Plex here"), plexFixture(), noGaze);
    CHECK(b.verb == EVerb::Clarify);
    CHECK(b.clarifyQuestion == "look at the target monitor");
}

TEST_CASE("intent: an unmatched window is still refused when the destination is 'here'") {
    CRuleIntent eng(icfg());
    SAction a = eng.resolve(mk("move the spreadsheet here"), plexFixture(), gazeAt(4, "XR-web"));
    CHECK(a.verb == EVerb::Clarify);
    CHECK(a.windowAddress.empty());
    CHECK_FALSE(a.actionable());
}

// The double deictic. The locked interaction model is content-first with ONE trailing
// deictic resolved at word time; "move this here" asks for two, at two different instants,
// for two different things. It is also exactly how the XR place verb is spoken, so reading
// it as a window move would steal "put it here" / "drop it here" from Place to guess at a
// window nobody named. Both decline, and "move this to this monitor" remains the way to
// move the focused window by gaze — one deictic, on the destination.
TEST_CASE("intent: a deictic subject next to a deictic destination is not a window move") {
    CRuleIntent eng(icfg());
    for (const char* said : {"move this here", "move it here", "send this over there"}) {
        SAction a = eng.resolve(mk(said), plexFixture(), gazeAt(4, "XR-web"));
        CHECK_MESSAGE(a.verb != EVerb::MoveWindow, said);
        CHECK_MESSAGE(!a.actionable(), said);
    }
    // The XR place verb keeps every one of its phrasings.
    CHECK(eng.detect(mk("put it here")).verb == EVerb::Place);
    CHECK(eng.detect(mk("place this here")).verb == EVerb::Place);
    CHECK(eng.detect(mk("drop it right here")).verb == EVerb::Place);
    // …including when it names its monitor: a subject that SAYS it is an output is the
    // place verb naming what it holds, not a window reference.
    CHECK(eng.detect(mk("put the coding monitor here")).verb == EVerb::Place);
    // And the single-deictic window move is untouched.
    SAction a = eng.resolve(mk("move this to this monitor"), plexFixture(), gazeAt(4, "XR-web"));
    CHECK(a.verb == EVerb::MoveWindow);
    CHECK(a.windowAddress.empty()); // "the one I am using"
    CHECK(a.target == "XR-web");
}

// ---- "move workspace N here" must never degrade to the SWITCH verb ------------------

TEST_CASE("intent: 'move workspace two here' is a workspace MOVE, not a switch") {
    CRuleIntent eng(icfg());
    // The verb is decided by the grammar alone — no gaze is consulted at parse time — so
    // the switch reading is gone whether or not the ring can answer.
    SRawIntent r = eng.detect(mk("move workspace two here"));
    CHECK(r.verb == EVerb::MoveWorkspace);
    CHECK(r.workspace == 2);
    CHECK(r.destDeictic);

    SAction a = eng.resolve(mk("move workspace two here"), plexFixture(), gazeAt(3, "XR-code"));
    CHECK(a.verb == EVerb::MoveWorkspace);
    CHECK(a.workspace == 2);
    CHECK(a.target == "XR-code");
    CHECK(a.targetSource == ETargetSource::Deixis);

    // With no usable gaze it ASKS. What it must never do is switch workspaces — the live
    // round did that three times in a row.
    SAction b = eng.resolve(mk("move workspace two here"), plexFixture(), noGaze);
    CHECK(b.verb == EVerb::Clarify);
    CHECK(b.clarifyQuestion == "look at the target monitor");
    CHECK_FALSE(b.actionable());
}

TEST_CASE("intent: 'move workspace N here' — digit, number word, and homophone") {
    CRuleIntent eng(icfg());
    struct { const char* said; int want; } cases[] = {
        {"move workspace 2 here",        2},
        {"move workspace two here",      2},
        {"move workspace to here",       2},  // Whisper's "two"
        {"move workspace too here",      2},
        {"move workspace four here",     4},
        {"move workspace 4 here",        4},
        {"move workspace for here",      4},  // Whisper's "four"
        {"move workspace forward here",  4},
        {"move workspace ate here",      8},
        {"move workspace won here",      1},
        {"move workspace tree here",     3},
        {"move work space three here",   3},  // the ASR's split compound
    };
    for (auto& c : cases) {
        SAction a = eng.resolve(mk(c.said), plexFixture(), gazeAt(3, "XR-code"));
        CHECK_MESSAGE(a.verb == EVerb::MoveWorkspace, c.said);
        CHECK_MESSAGE(a.workspace == c.want, c.said);
        CHECK_MESSAGE(a.target == "XR-code", c.said);
    }
}

TEST_CASE("intent: a trailing 'here' means the move VERB was lost, not that a switch was meant") {
    // "Remove workspace 4 here." — Whisper's rendering of "move". No verb the move parser
    // recognises survives, but the destination remnant does, and the switch verb has
    // nowhere to put it.
    CRuleIntent eng(icfg());
    SRawIntent r = eng.detect(mk("remove workspace 4 here"));
    CHECK(r.verb == EVerb::MoveWorkspace);
    CHECK(r.workspace == 4);

    SAction a = eng.resolve(mk("remove workspace 4 here"), plexFixture(), gazeAt(4, "XR-web"));
    CHECK(a.verb == EVerb::MoveWorkspace);
    CHECK(a.target == "XR-web");

    // An EXPLICIT switch marker still names its own verb — a stray deictic must not
    // override a verb the user actually spoke.
    CHECK(eng.detect(mk("go to workspace three")).verb == EVerb::Workspace);
    CHECK(eng.detect(mk("switch to workspace three here")).verb == EVerb::Workspace);
    // …and an ordinary switch is untouched.
    CHECK(eng.detect(mk("workspace three")).verb == EVerb::Workspace);
    // A deictic that is NOT trailing is not a destination remnant.
    CHECK(eng.detect(mk("put this here on workspace 3")).verb != EVerb::MoveWorkspace);
}

// ---------------------------------------------------------------------------
// Round 6 addendum: destination prepositions beyond "to".
//
// Live fire: "Move workspace forward IN this monitor." degraded to a workspace switch —
// the destination grammar accepted only "to", so the move parse failed and the switch
// verb swallowed the phrase and dropped the destination. People say in / on / onto / at
// interchangeably here, and every one of them still has to lead to something that NAMES
// an output, so the wider set cannot invent a target.
// ---------------------------------------------------------------------------

TEST_CASE("intent: every destination preposition reaches the same monitor") {
    CRuleIntent eng(icfg());
    for (const char* said : {"move workspace 4 to this monitor",
                             "move workspace 4 in this monitor",
                             "move workspace 4 on this monitor",
                             "move workspace 4 onto this monitor",
                             "move workspace 4 at this monitor",
                             "move workspace 4 over to this monitor"}) {
        SAction a = eng.resolve(mk(said), plexFixture(), gazeAt(3, "XR-code"));
        CHECK_MESSAGE(a.verb == EVerb::MoveWorkspace, said);
        CHECK_MESSAGE(a.workspace == 4, said);
        CHECK_MESSAGE(a.target == "XR-code", said);
    }
    // The exact live utterance, with "forward" for "four".
    SAction live = eng.resolve(mk("move workspace forward in this monitor"), plexFixture(),
                               gazeAt(3, "XR-code"));
    CHECK(live.verb == EVerb::MoveWorkspace);
    CHECK(live.workspace == 4);
    CHECK(live.target == "XR-code");

    // …and the same for a window move, against a named/spatial destination.
    for (const char* said : {"move Plex on the left monitor", "move Plex in the left monitor",
                             "put Plex at the left monitor", "send Plex onto the left monitor"}) {
        SAction a = eng.resolve(mk(said), plexFixture(), noGaze);
        CHECK_MESSAGE(a.verb == EVerb::MoveWindow, said);
        CHECK_MESSAGE(a.windowAddress == "0xp1ex", said);
        CHECK_MESSAGE(a.target == "eDP-1", said); // leftmost by layout x
    }
}

TEST_CASE("intent: a workspace phrase with ANY destination remnant never switches") {
    CRuleIntent eng(icfg());
    // The move verb was lost ("remove"), but the destination survived. Whatever the
    // preposition, the one outcome that must not happen is a silent workspace switch.
    for (const char* said : {"remove workspace 4 here",
                             "remove workspace 4 to this monitor",
                             "remove workspace 4 in this monitor",
                             "remove workspace 4 on this monitor",
                             "remove workspace 4 at this monitor",
                             "workspace 4 in this monitor"}) {
        SRawIntent r = eng.detect(mk(said));
        CHECK_MESSAGE(r.verb == EVerb::MoveWorkspace, said);
        CHECK_MESSAGE(r.workspace == 4, said);
        SAction a = eng.resolve(mk(said), plexFixture(), gazeAt(4, "XR-web"));
        CHECK_MESSAGE(a.verb == EVerb::MoveWorkspace, said);
        CHECK_MESSAGE(a.target == "XR-web", said);
        // …and with no destination resolvable it ASKS rather than switching.
        SAction b = eng.resolve(mk(said), plexFixture(), noGaze);
        CHECK_MESSAGE(b.verb == EVerb::Clarify, said);
        CHECK_MESSAGE(!b.actionable(), said);
    }
    // A spatial remnant resolves against the layout, exactly like a parsed move.
    SAction sp = eng.resolve(mk("remove workspace 4 on the left monitor"), plexFixture(), noGaze);
    CHECK(sp.verb == EVerb::MoveWorkspace);
    CHECK(sp.target == "eDP-1");

    // A workspace phrase with NO destination is still a plain switch.
    CHECK(eng.detect(mk("workspace three")).verb == EVerb::Workspace);
    CHECK(eng.detect(mk("go to workspace three")).verb == EVerb::Workspace);
    CHECK(eng.detect(mk("move to workspace three")).verb == EVerb::Workspace);
    CHECK(eng.detect(mk("switch to workspace 4 on this monitor")).verb == EVerb::Workspace);
}

// ===========================================================================
// Round 8 — deixis semantics. Two live misfires drove this block:
//
//   1. "Move this monitor closer."  → verb=move_dist target=active src=active with NO
//      gaze block at all. The MoveDist branch cleared `deictic` wholesale ("closer is
//      motion, not a place-deixis"), which also threw away the MONITOR-deixis "this";
//      every word of the utterance is a command word, so the target phrase was empty
//      too, and finalize's last resort emitted `active`. The compositor then pulled
//      whatever resolveSelected() pointed at — never the monitor being looked at.
//
//   2. "Create a monitor here."     → placed where the user looked ~half a second
//      BEFORE the word. Every live create logged ageMs ≈ 500 = deixis_lead_ms(200) +
//      deixis_window_ms(300): the resolver picked the OLDEST sample in the stability
//      window as its representative pose, because a deixis aimed at passthrough reports
//      dwellSec = 0.000 in every sample and the "highest dwell" tie-break fell through
//      to first-pushed. Fixed in GazeResolver (see test_gaze.cpp).
//
// The contract these pin down is in docs/DEIXIS-SEMANTICS.md.
// ===========================================================================

namespace {
    // A ring that answers with DIFFERENT monitors either side of `splitMs`: `before` for
    // samples at or before it, `after` for anything later. Every word-time assertion in
    // this block works by putting the word on one side and "now" on the other.
    GazeQueryFn gazeSplit(int64_t splitMs, int beforeId, const char* beforeName,
                          int afterId, const char* afterName) {
        return [=](int64_t at) {
            const int   id = at <= splitMs ? beforeId : afterId;
            const char* nm = at <= splitMs ? beforeName : afterName;
            char        b[360];
            std::snprintf(b, sizeof(b),
                          R"json({"ok":true,"viewValid":true,"timestampMs":%lld,
                          "head":{"pos":[0,1.4,0],"forward":[0,0,-1]},
                          "gaze":{"monitorId":%d,"name":"%s","selected":%s,"dwellSec":0.3},
                          "query":{"matchedTimestampMs":%lld,"ageMs":0}})json",
                          (long long)at, id, id >= 0 ? nm : "", id >= 0 ? "true" : "false",
                          (long long)at);
            return std::string(b);
        };
    }
}

TEST_CASE("intent: 'move this monitor closer' targets the GAZED monitor, never active") {
    CRuleIntent eng(icfg());

    // The deixis survives the MoveDist branch.
    SRawIntent r = eng.detect(mk("move this monitor closer"));
    CHECK(r.verb == EVerb::MoveDist);
    CHECK(r.deictic);
    CHECK_FALSE(r.deicticIsPlace);
    CHECK(r.deltaM < 0);

    SAction a = eng.resolve(mk("move this monitor closer"), fixture(), gazeAt(4, "XR-web"));
    CHECK(a.verb == EVerb::MoveDist);
    CHECK(a.target == "XR-web");
    CHECK(a.targetSource == ETargetSource::Deixis);
    CHECK(a.gaze.valid);
    CHECK(a.deltaM == doctest::Approx(-0.25));

    // …and the other direction, and the bare-subject phrasing, resolve the same way.
    SAction b = eng.resolve(mk("push this monitor further away"), fixture(), gazeAt(3, "XR-code"));
    CHECK(b.verb == EVerb::MoveDist);
    CHECK(b.target == "XR-code");
    CHECK(b.targetSource == ETargetSource::Deixis);
    CHECK(b.deltaM > 0);

    SAction c = eng.resolve(mk("move this closer"), fixture(), gazeAt(3, "XR-code"));
    CHECK(c.verb == EVerb::MoveDist);
    CHECK(c.target == "XR-code");
    CHECK(c.targetSource == ETargetSource::Deixis);
}

TEST_CASE("intent: a move_dist deixis is resolved at the WORD, not at 'now'") {
    CRuleIntent eng(icfg());
    // mk() puts words 200 ms apart from t0: move@100000 this@100200 monitor@100400
    // closer@100600. The deictic word is "this" at 100200; with the default lead (200)
    // and window (300) every sample lands in [99700, 100000]. Whisper decode then takes
    // 1.5-3 s, so a query issued "now" would be somewhere past 102000 — and by then the
    // user is looking at XR-code, not the XR-web they meant.
    GazeQueryFn q = gazeSplit(100100, 4, "XR-web", 3, "XR-code");
    SAction a = eng.resolve(mk("move this monitor closer"), fixture(), q);
    CHECK(a.target == "XR-web");             // where they looked when they said "this"
    CHECK(a.targetSource == ETargetSource::Deixis);
    CHECK(a.gaze.requestedMs == 100200);     // the word's DTW timestamp, verbatim

    // The lead shift is deliberate and bounded: the pose comes from lead_ms before the
    // word, NOT from lead_ms + window_ms before it (the round-8 rep-selection bug).
    CHECK(a.gaze.ageMs == icfg().gaze.leadMs);
}

TEST_CASE("intent: a create-here place point is resolved at the WORD, not at 'now'") {
    CRuleIntent eng(icfg());
    // create@100000 a@100200 monitor@100400 here@100600 -> anchor 100400, window to 100100.
    GazeQueryFn q = gazeSplit(100500, -1, "", -1, "");
    SAction a = eng.resolve(mk("create a monitor here"), btopFixture(), q);
    CHECK(a.verb == EVerb::CreateMonitor);
    CHECK(a.gaze.valid);
    CHECK(a.gaze.requestedMs == 100600);          // the word "here", not utterance end
    CHECK(a.gaze.ageMs == icfg().gaze.leadMs);    // and not leadMs + windowMs
    CHECK(a.gaze.placeDistM > 0.0);
}

TEST_CASE("intent: 'move this monitor closer' with nothing under gaze ASKS") {
    CRuleIntent eng(icfg());
    // The user said "this", so they were looking at something specific. Every fallback we
    // have (pointer hover, sticky selection, keyboard focus) resolves to somewhere they
    // were NOT looking — note the fixture's XR-web carries hovered:true, and the old code
    // would have silently taken it.
    SAction a = eng.resolve(mk("move this monitor closer"), fixture(), gazeAt(-1, ""));
    CHECK(a.verb == EVerb::Clarify);
    CHECK(a.clarifyQuestion == "look at the monitor you mean");
    CHECK_FALSE(a.actionable());

    // No ring at all is the same answer, not a guess.
    SAction b = eng.resolve(mk("pick this monitor up"), fixture(), noGaze);
    CHECK(b.verb == EVerb::Clarify);
    CHECK_FALSE(b.actionable());
}

TEST_CASE("intent: a place-deixis in a move_dist phrase is motion, not a target") {
    CRuleIntent eng(icfg());
    // "come here" / "bring it here" name no monitor and no point — they are the direction
    // word said twice. Dropping the place-deixis here is what the old flat
    // `r.deictic = false` was for, and that part was right.
    for (const char* said : {"come here", "bring it closer, come here"}) {
        SRawIntent r = eng.detect(mk(said));
        CHECK_MESSAGE(r.verb == EVerb::MoveDist, said);
        CHECK_MESSAGE(!r.deictic, said);
        CHECK_MESSAGE(r.deicticWordMs == 0, said);
    }
}

TEST_CASE("intent: a bare 'move closer' uses the monitor gazed at utterance onset") {
    CRuleIntent eng(icfg());
    // No subject was spoken at all. Round-8 decision: gaze-first. The monitor you are
    // LOOKING at is what a bare direction means far more often than whatever `openxr
    // select` last stuck to — the compositor's selection is sticky and can be minutes old.
    // With no deictic word to time the query from, the utterance ONSET is used.
    SAction a = eng.resolve(mk("move closer"), fixture(), gazeAt(4, "XR-web"));
    CHECK(a.verb == EVerb::MoveDist);
    CHECK(a.target == "XR-web");
    CHECK(a.targetSource == ETargetSource::Deixis);
    CHECK(a.gaze.requestedMs == 100000); // mk()'s onset

    // Gaze says nothing -> the historical hover/selection fallback, and it is labelled
    // Active because that is what it is (tier 2 of the compositor's resolveSelected()).
    SAction b = eng.resolve(mk("move closer"), fixture(), gazeAt(-1, ""));
    CHECK(b.verb == EVerb::MoveDist);
    CHECK(b.target == "XR-web");         // fixture's hovered monitor
    CHECK(b.targetSource == ETargetSource::Active);

    // A named subject still beats the gaze, as everywhere else.
    SAction c = eng.resolve(mk("move the coding monitor closer"), fixture(), gazeAt(4, "XR-web"));
    CHECK(c.target == "XR-code");
    CHECK(c.targetSource == ETargetSource::Semantic);
}

TEST_CASE("intent: only a SPOKEN 'active'/'focused' resolves to target=active") {
    CRuleIntent eng(icfg());
    for (const char* said : {"move the active monitor closer", "move the focused screen closer",
                             "move the current monitor closer"}) {
        // Even with a perfectly good gaze pick available, the spoken word wins: the user
        // asked for the compositor's selection by name.
        SAction a = eng.resolve(mk(said), fixture(), gazeAt(4, "XR-web"));
        CHECK_MESSAGE(a.verb == EVerb::MoveDist, said);
        CHECK_MESSAGE(a.target == "active", said);
        CHECK_MESSAGE(a.targetSource == ETargetSource::Active, said);
    }
    // "the active coding monitor" has a content word in it — it names XR-code.
    SAction b = eng.resolve(mk("move the active coding monitor closer"), fixture(), noGaze);
    CHECK(b.target == "XR-code");
    CHECK(b.targetSource == ETargetSource::Semantic);
}

TEST_CASE("intent: 'put the coding monitor here' keeps BOTH the name and the place point") {
    CRuleIntent eng(icfg());
    // A place-deixis designates a POINT; it never competes with the subject for the name
    // slot. The old wantDeixis gate suppressed the gaze query whenever a confident name
    // was matched, so this utterance silently lost its "here" and degraded to a
    // freeze-in-place on whatever the compositor had selected.
    SAction a = eng.resolve(mk("put the coding monitor here"), fixture(), gazeAt(-1, ""));
    CHECK(a.verb == EVerb::Place);
    CHECK(a.target == "XR-code");
    CHECK(a.targetSource == ETargetSource::Semantic);
    CHECK(a.gaze.valid);
    CHECK(a.gaze.placeDistM > 0.0);

    // The anaphoric subject ("it" — the thing you are carrying) still defers to the
    // compositor's selection, which for a place is the right answer rather than a guess.
    SAction b = eng.resolve(mk("place it here"), fixture(), gazeAt(-1, ""));
    CHECK(b.target == "active");
    CHECK(b.targetSource == ETargetSource::Deixis);
    CHECK(b.gaze.placeDistM > 0.0);
}
