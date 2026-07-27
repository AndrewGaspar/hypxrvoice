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

TEST_CASE("intent: sanitizeDeltaM — utterance direction is authoritative, magnitude clamped") {
    // The live bug: model said +100 for "closer" — direction flipped, magnitude reset.
    CHECK(sanitizeDeltaM(100.0, "move the coding monitor closer", 0.25) == doctest::Approx(-0.25));
    // Sane model value with matching direction passes through (sign from utterance).
    CHECK(sanitizeDeltaM(-0.1, "bring it closer", 0.25) == doctest::Approx(-0.1));
    CHECK(sanitizeDeltaM(0.1, "push it further away", 0.25) == doctest::Approx(0.1));
    // Model sign disagrees with the utterance: utterance wins.
    CHECK(sanitizeDeltaM(0.3, "a bit closer please", 0.25) == doctest::Approx(-0.3));
    // No direction word: model sign trusted, magnitude clamped.
    CHECK(sanitizeDeltaM(-50.0, "adjust the distance", 0.25) == doctest::Approx(-0.25));
    // Zero/absent value: step magnitude, default pull.
    CHECK(sanitizeDeltaM(0.0, "adjust it", 0.25) == doctest::Approx(-0.25));
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
    CHECK(eng.detect(mk("put the editor here")).verb == EVerb::Place);
    CHECK(eng.detect(mk("bring it closer")).verb == EVerb::MoveDist);
    CHECK(eng.detect(mk("push it further away")).verb == EVerb::MoveDist);
}
