#include "doctest.h"

#include "Config.hpp"
#include "Pipeline.hpp"
#include "VocabBias.hpp"

#include <algorithm>
#include <cmath>

namespace {
    bool contains(const std::string& hay, const std::string& needle) {
        return hay.find(needle) != std::string::npos;
    }
    bool hasTerm(const std::vector<std::string>& v, const std::string& t) {
        return std::find(v.begin(), v.end(), t) != v.end();
    }

    // The user's ACTUAL desktop on the night the mangling was reported (from
    // `hyprctl clients -j`): Plex, a WhatsApp web app, five ghostty terminals whose
    // identity lives entirely in their titles, and two Chrome windows.
    const char* kMonitors = R"json([
        {"id":0,"name":"XR-main","focused":true,"x":0,"y":0,"width":2048,"height":1152,"activeWorkspace":{"name":"1"}},
        {"id":2,"name":"XR-2","focused":false,"x":2048,"y":0,"width":2048,"height":1152,"activeWorkspace":{"name":"2"}},
        {"id":3,"name":"XR-3","focused":false,"x":4096,"y":0,"width":2048,"height":1152,"activeWorkspace":{"name":"3"}}
    ])json";
    const char* kClients = R"json([
        {"address":"0x1006","class":"chrome-x.com__-Default","title":"Home / X","monitor":3,"mapped":true,"focusHistoryID":6},
        {"address":"0x1005","class":"google-chrome","title":"Plex - ArchWiki - Google Chrome","monitor":3,"mapped":true,"focusHistoryID":5},
        {"address":"0x1001","class":"Plex","title":"Plex","monitor":2,"mapped":true,"focusHistoryID":1},
        {"address":"0x1002","class":"com.mitchellh.ghostty","title":"btop","monitor":0,"mapped":true,"focusHistoryID":2},
        {"address":"0x1003","class":"com.mitchellh.ghostty","title":"nvtop","monitor":0,"mapped":true,"focusHistoryID":3},
        {"address":"0x1004","class":"chrome-web.whatsapp.com__-Default","title":"web.whatsapp.com","monitor":3,"mapped":true,"focusHistoryID":4},
        {"address":"0x1007","class":"com.mitchellh.ghostty","title":"nvim ~/.config/hypxrvoice/config.toml ","monitor":0,"mapped":true,"focusHistoryID":7},
        {"address":"0x1008","class":"com.mitchellh.ghostty","title":"~/code/Hyprland","monitor":0,"mapped":true,"focusHistoryID":8}
    ])json";

    SDesktopContext fixture() { return SDesktopContext::parse(kMonitors, kClients, ""); }
}

TEST_CASE("vocab bias: a class is reduced to the name a person would say") {
    // Reverse-DNS: the identity is the LAST label.
    CHECK(VocabBias::spokenClassName("com.mitchellh.ghostty") == "ghostty");
    CHECK(VocabBias::spokenClassName("org.gnome.Nautilus") == "Nautilus");
    // Browser web-app wrapper: strip the wrapper AND the chrome profile suffix, then
    // take the domain's second-level label — not "web", not "com".
    CHECK(VocabBias::spokenClassName("chrome-web.whatsapp.com__-Default") == "whatsapp");
    CHECK(VocabBias::spokenClassName("chromium-app.slack.com__-Profile_1") == "slack");
    // Vendor-hyphenated: the app is last.
    CHECK(VocabBias::spokenClassName("google-chrome") == "chrome");
    // Already a spoken name — casing is preserved, because "Plex" is how it is written.
    CHECK(VocabBias::spokenClassName("Plex") == "Plex");
    CHECK(VocabBias::spokenClassName("firefox") == "firefox");
    // Nothing usable: a bare TLD, a too-short label, digits, empty.
    CHECK(VocabBias::spokenClassName("chrome-x.com__-Default").empty());
    CHECK(VocabBias::spokenClassName("com").empty());
    CHECK(VocabBias::spokenClassName("1234").empty());
    CHECK(VocabBias::spokenClassName("").empty());
}

TEST_CASE("vocab bias: title terms take the name, not the path") {
    // The salient name leads; the path behind it is noise nobody speaks.
    std::vector<std::string> t = VocabBias::titleTerms("nvim ~/.config/hypxrvoice/config.toml ", "ghostty", 3);
    REQUIRE(t.size() == 1);
    CHECK(t[0] == "nvim");
    // A pure path yields nothing at all.
    CHECK(VocabBias::titleTerms("~/code/Hyprland", "ghostty", 3).empty());
    // Casing is kept as written on screen.
    std::vector<std::string> p = VocabBias::titleTerms("Plex - ArchWiki - Google Chrome", "chrome", 2);
    REQUIRE(p.size() >= 1);
    CHECK(p[0] == "Plex");
    // The window's own class term is not repeated, and address labels never qualify.
    std::vector<std::string> w = VocabBias::titleTerms("web.whatsapp.com", "whatsapp", 3);
    CHECK(w.empty());
    // Stop-words and short tokens are dropped.
    CHECK(VocabBias::titleTerms("the new tab", "", 3).empty());
    // So are identifiers — a token mixing letters and digits is a hash or a build id,
    // never something anyone says. The live desktop offered "RaldS7" as vocabulary.
    CHECK(VocabBias::titleTerms("RaldS7 a1b2c3 2026", "", 3).empty());
    std::vector<std::string> mix = VocabBias::titleTerms("btop RaldS7 nvtop", "", 3);
    REQUIRE(mix.size() == 2);
    CHECK(mix[0] == "btop");
    CHECK(mix[1] == "nvtop");
}

TEST_CASE("vocab bias: the live vocabulary is what the user actually says") {
    SVocabBiasConfig cfg;
    std::vector<std::string> v = VocabBias::terms(fixture(), cfg);

    // The proper nouns whisper mangled live, all present.
    CHECK(hasTerm(v, "Plex"));
    CHECK(hasTerm(v, "whatsapp"));
    CHECK(hasTerm(v, "ghostty"));
    CHECK(hasTerm(v, "chrome"));
    // ...including the two that exist ONLY in a title. Round-robin over windows is what
    // gets them in: taking two tokens from the first terminal instead spent the budget
    // on its own title and btop never made the prompt.
    CHECK(hasTerm(v, "btop"));
    CHECK(hasTerm(v, "nvtop"));
    CHECK(hasTerm(v, "nvim"));
    // Monitor names come along.
    CHECK(hasTerm(v, "XR-main"));
    CHECK(hasTerm(v, "XR-2"));
    // Path debris does not.
    CHECK_FALSE(hasTerm(v, "config"));
    CHECK_FALSE(hasTerm(v, "toml"));
    CHECK_FALSE(hasTerm(v, "web"));
    // No duplicates.
    for (size_t i = 0; i < v.size(); i++)
        for (size_t j = i + 1; j < v.size(); j++)
            CHECK(v[i] != v[j]);
}

TEST_CASE("vocab bias: focus recency decides who survives the cap") {
    SVocabBiasConfig cfg;
    cfg.maxTerms    = 2;
    cfg.maxMonitors = 0;
    std::vector<std::string> v = VocabBias::terms(fixture(), cfg);
    REQUIRE(v.size() == 2);
    // focusHistoryID 1 is Plex, 2 is a ghostty — the two most recently focused windows.
    CHECK(v[0] == "Plex");
    CHECK(v[1] == "ghostty");
}

TEST_CASE("vocab bias: the built prompt is bounded, deterministic, and command-shaped") {
    SVocabBiasConfig cfg;
    const std::string a = VocabBias::build(fixture(), cfg);
    const std::string b = VocabBias::build(fixture(), cfg);
    CHECK(a == b); // same desktop in, same prompt out

    // Exemplar COMMANDS, not a glossary — measured at 10/12 exact matches on the
    // degraded-speech corpus versus 4/12 for a bare comma list. See VocabBias.hpp.
    CHECK(contains(a, "Move Plex here."));
    CHECK(contains(a, "btop"));
    CHECK(contains(a, "nvtop"));
    CHECK_FALSE(contains(a, "Plex, "));

    // The fixed tail is always there: the command verbs plus a spelled-out workspace
    // number (the live "four" -> "for"/"forward" failure).
    CHECK(contains(a, "Move workspace four to this monitor."));
    CHECK(contains(a, "Make this window fullscreen."));
    // ...and the tail carries NOTHING that only duplicates the rotating templates. The
    // sentence that used to sit here, "Focus the browser.", was measured driving whisper
    // into a 55x repetition loop on a real recording of the user saying exactly that —
    // 1.2 s of decode became 5.8 s of temperature-fallback repair. See VocabBias.cpp.
    CHECK_FALSE(contains(a, "Focus the browser."));

    // Bounded: the cap on terms bounds the prompt.
    cfg.maxTerms       = 2;
    cfg.maxMonitors    = 1;
    const std::string small = VocabBias::build(fixture(), cfg);
    CHECK(small.size() < a.size());
    CHECK(contains(small, "Move workspace four to this monitor."));

    // Disabled produces nothing at all.
    cfg.enabled = false;
    CHECK(VocabBias::build(fixture(), cfg).empty());
}

TEST_CASE("vocab bias: an empty desktop still biases the command verbs") {
    SDesktopContext   empty;
    SVocabBiasConfig  cfg;
    const std::string o = VocabBias::build(empty, cfg);
    CHECK_FALSE(o.empty());
    CHECK(contains(o, "workspace four"));
    CHECK(contains(o, "fullscreen"));
}

// ---- the hallucination guard ----------------------------------------------------

namespace {
    // n ms of 16 kHz audio: `amp` is the peak of a 200 Hz tone (0 = silence).
    SSpeechSegment tone(int ms, float amp) {
        SSpeechSegment s;
        const size_t   n = static_cast<size_t>(ms) * 16;
        s.samples.resize(n);
        for (size_t i = 0; i < n; i++)
            s.samples[i] = amp * std::sin(2.0 * 3.14159265 * 200.0 * static_cast<double>(i) / 16000.0);
        s.bufferStartMs = 1000;
        s.onsetMs       = 1000;
        s.endMs         = 1000 + ms;
        return s;
    }
}

TEST_CASE("vocab bias guard: near-silence is transcribed WITHOUT the prompt") {
    SConfig cfg; // defaults: vocab_bias on, min_voiced_ms 200

    // A confidently-voiced buffer is biased.
    CHECK(Pipeline::vocabBiasAllowed(cfg, tone(1000, 0.2f)));

    // Silence is not. This is the whole guard: an initial prompt makes whisper
    // confident, and on ambient it spends that confidence inventing fluent text while
    // driving its own no_speech_prob to zero — so whisper's no-speech verdict cannot
    // police the prompt, and the audio has to be measured first.
    CHECK_FALSE(Pipeline::vocabBiasAllowed(cfg, tone(2000, 0.0f)));
    // Neither is a buffer with a blip too short to be a command.
    CHECK_FALSE(Pipeline::vocabBiasAllowed(cfg, tone(60, 0.2f)));
    // Nor an empty one.
    CHECK_FALSE(Pipeline::vocabBiasAllowed(cfg, SSpeechSegment{}));

    // Config plumbing: the feature switch and the guard threshold are both honoured.
    SConfig off = cfg;
    off.asr.vocabBias = false;
    CHECK_FALSE(Pipeline::vocabBiasAllowed(off, tone(1000, 0.2f)));

    SConfig nogate = cfg;
    nogate.asr.vocabBiasMinVoicedMs = 0; // guard explicitly disabled
    CHECK(Pipeline::vocabBiasAllowed(nogate, tone(2000, 0.0f)));

    SConfig strict = cfg;
    strict.asr.vocabBiasMinVoicedMs = 5000;
    CHECK_FALSE(Pipeline::vocabBiasAllowed(strict, tone(1000, 0.2f)));
}

TEST_CASE("vocab bias guard: a replayed prompt is rejected, a real command is not") {
    const std::string prompt =
        "Move Plex here. Focus ghostty. Move whatsapp to this monitor. "
        "Move workspace four to this monitor. Make this window fullscreen.";

    // The decoder replaying a stretch of its own context is not a transcript.
    CHECK(Pipeline::isPromptEcho("Move Plex here. Focus ghostty. Move whatsapp to this monitor.", prompt));
    // Re-punctuated / re-cased echoes count too — whisper rewrites both freely.
    CHECK(Pipeline::isPromptEcho("move plex here, focus ghostty, move whatsapp to this monitor", prompt));

    // What must NOT be rejected: the user really saying one of the biased phrases. That
    // is the entire point of the feature, so the guard only fires on a long run.
    CHECK_FALSE(Pipeline::isPromptEcho("Move Plex here.", prompt));
    CHECK_FALSE(Pipeline::isPromptEcho("Plex", prompt));
    CHECK_FALSE(Pipeline::isPromptEcho("Focus ghostty", prompt));
    // Anything not in the prompt at all is untouched, however long.
    CHECK_FALSE(Pipeline::isPromptEcho("Move the browser to the left monitor and make it fullscreen", prompt));
    // No prompt installed: never an echo.
    CHECK_FALSE(Pipeline::isPromptEcho("Move Plex here. Focus ghostty. Move whatsapp to this monitor.", ""));
}
