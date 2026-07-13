#include "doctest.h"

#include "Grammar.hpp"
#include "LlamaIntent.hpp"

#include <string>

// The GBNF grammar + raw-intent parsing + prompt building are PURE and compile
// regardless of HAVE_LLAMA; the model decode loop itself is smoke-tested live.

TEST_CASE("grammar: enumerates live monitor names and the closed verb set") {
    std::string g = Grammar::buildIntentGrammar({"XR-code", "XR-web"});
    // Each monitor name appears as a quoted literal alternative for `target`.
    CHECK(g.find("XR-code") != std::string::npos);
    CHECK(g.find("XR-web") != std::string::npos);
    // The verb rule lists the schema verbs.
    CHECK(g.find("move_dist") != std::string::npos);
    CHECK(g.find("launch_app") != std::string::npos);
    CHECK(g.find("root ::=") != std::string::npos);
    // "active" is always an allowed target.
    CHECK(g.find("active") != std::string::npos);
}

TEST_CASE("grammar: with no monitors the target still allows active/empty") {
    std::string g = Grammar::buildIntentGrammar({});
    CHECK(g.find("target ::=") != std::string::npos);
    CHECK(g.find("active") != std::string::npos);
}

TEST_CASE("llama: parseRaw maps a model object to a raw intent") {
    const char* json = R"json({
        "verb":"move_dist","target":"XR-code","deictic":false,"place":false,
        "anchor":"","sub":"","deltaM":-0.25,"app":"","confidence":0.8
    })json";
    SRawIntent r = CLlamaIntent::parseRaw(json, 12345, false);
    CHECK(r.verb == EVerb::MoveDist);
    CHECK(r.targetPhrase == "XR-code");
    CHECK(r.deltaM == doctest::Approx(-0.25));
    CHECK(r.confidence == doctest::Approx(0.8));
    CHECK_FALSE(r.deictic);
}

TEST_CASE("llama: parseRaw carries the transcript's deictic timestamp") {
    const char* json = R"json({
        "verb":"pick","target":"active","deictic":true,"place":false,
        "anchor":"","sub":"","deltaM":0,"app":"","confidence":1.0
    })json";
    SRawIntent r = CLlamaIntent::parseRaw(json, 99000, false);
    CHECK(r.verb == EVerb::Pick);
    CHECK(r.deictic);
    CHECK(r.deicticWordMs == 99000);
    CHECK(r.targetPhrase.empty()); // "active" is not a semantic phrase
}

TEST_CASE("llama: parseRaw on launch fills the app phrase") {
    const char* json = R"json({
        "verb":"launch_app","target":"","deictic":false,"place":false,
        "anchor":"","sub":"","deltaM":0,"app":"browser","confidence":0.9
    })json";
    SRawIntent r = CLlamaIntent::parseRaw(json, 0, false);
    CHECK(r.verb == EVerb::LaunchApp);
    CHECK(r.appPhrase == "browser");
}

TEST_CASE("llama: parseRaw on garbage yields a safe None") {
    SRawIntent r = CLlamaIntent::parseRaw("not json", 0, false);
    CHECK(r.verb == EVerb::None);
}

TEST_CASE("llama: buildPrompt injects the digest and the transcript") {
    const char* mons = R"json([{"id":3,"name":"XR-code"}])json";
    SDesktopContext ctx = SDesktopContext::parse(mons, "", "");
    STranscript t; t.text = "move the coding monitor closer";
    CLlamaIntent eng; // not loaded — buildPrompt is pure
    SIntentConfig ic;
    std::string p = eng.buildPrompt(t, ctx, ic);
    CHECK(p.find("XR-code") != std::string::npos);
    CHECK(p.find("move the coding monitor closer") != std::string::npos);
    CHECK(p.find("Verbs:") != std::string::npos);
}
