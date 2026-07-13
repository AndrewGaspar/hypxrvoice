#include "doctest.h"

#include "Tts.hpp"

namespace {
    SConfig cfgMode(const char* mode) {
        SConfig c;
        c.feedback.ttsMode = mode;
        return c;
    }
}

TEST_CASE("tts: off mode is always silent") {
    SConfig cfg = cfgMode("off");
    SAction a; a.verb = EVerb::Anchor; a.target = "XR-code";
    SExecPlan ok; ok.ok = true;
    CHECK(Tts::phraseFor(a, ok, cfg).empty());

    SAction err; err.verb = EVerb::Anchor;
    SExecPlan bad; bad.ok = false; bad.reason = "no such monitor";
    CHECK(Tts::phraseFor(err, bad, cfg).empty());
}

TEST_CASE("tts: errors mode speaks refusals and clarify, not successes") {
    SConfig cfg = cfgMode("errors");

    SAction ok; ok.verb = EVerb::Anchor; ok.target = "XR-code";
    SExecPlan okp; okp.ok = true;
    CHECK(Tts::phraseFor(ok, okp, cfg).empty()); // success is visible on the HUD

    SAction err; err.verb = EVerb::Anchor; err.target = "XR-ghost";
    SExecPlan bad; bad.ok = false; bad.reason = "no such monitor";
    CHECK(Tts::phraseFor(err, bad, cfg) == "can't, no such monitor");

    SAction clar; clar.verb = EVerb::Clarify; clar.clarifyQuestion = "which firefox?";
    SExecPlan any;
    CHECK(Tts::phraseFor(clar, any, cfg) == "which firefox?");
}

TEST_CASE("tts: all mode also confirms successful actions terse-ly") {
    SConfig cfg = cfgMode("all");
    SAction ok; ok.verb = EVerb::MoveDist; ok.target = "XR-web"; ok.deltaM = -0.25;
    SExecPlan okp; okp.ok = true;
    CHECK(Tts::phraseFor(ok, okp, cfg) == "closer — XR-web");
}

TEST_CASE("tts: none verb never speaks") {
    for (const char* m : {"off", "errors", "all"}) {
        SConfig cfg = cfgMode(m);
        SAction a; a.verb = EVerb::None;
        SExecPlan p; p.ok = true;
        CHECK(Tts::phraseFor(a, p, cfg).empty());
    }
}

TEST_CASE("tts: refusal with empty reason still yields a terse phrase") {
    SConfig cfg = cfgMode("errors");
    SAction a; a.verb = EVerb::Dock; a.target = "XR-code";
    SExecPlan bad; bad.ok = false; // no reason text
    CHECK(Tts::phraseFor(a, bad, cfg) == "can't, not possible");
}
