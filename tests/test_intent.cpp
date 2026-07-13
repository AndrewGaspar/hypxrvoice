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
