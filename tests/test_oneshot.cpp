#include "doctest.h"

#include "Asr.hpp"
#include "Clock.hpp"
#include "Config.hpp"
#include "Pipeline.hpp"
#include "Vad.hpp"
#include "Wav.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace {
    // Locate a whisper model: $HYPXRVOICE_TEST_MODEL, else <source>/models/ggml-base.en.bin.
    std::string findModel() {
        if (const char* e = std::getenv("HYPXRVOICE_TEST_MODEL"); e && *e && std::filesystem::exists(e))
            return e;
        std::string def = std::string(HYPXRVOICE_SOURCE_DIR) + "/models/ggml-base.en.bin";
        if (std::filesystem::exists(def))
            return def;
        return "";
    }

    std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }
}

// Full transcript-tier pipeline over a real speech WAV (public-domain JFK clip).
// This is the acceptance test for WP-V2's deliverable: onset + WORD-LEVEL absolute
// monotonic timestamps out of the --oneshot path.
TEST_CASE("oneshot: JFK WAV yields a transcript with monotonic per-word timestamps") {
    std::string model = findModel();
    if (model.empty()) {
        MESSAGE("no whisper model found (run scripts/fetch-models.sh); skipping ASR pipeline test");
        return;
    }

    CAsr          asr;
    std::string   err;
    CAsr::SParams ap{model, "en", 4, false};
    REQUIRE_MESSAGE(asr.load(ap, err), err);

    std::vector<float> audio;
    std::string        werr;
    REQUIRE_MESSAGE(loadAudioMono16k(std::string(HYPXRVOICE_TEST_ASSETS) + "/jfk.wav", audio, werr), werr);
    REQUIRE(audio.size() > 16000); // > 1 s

    SConfig    cfg;
    SVadConfig vc;
    vc.sampleRate = 16000;
    // The JFK clip opens softly; loosen the gate a touch so onset is caught early.
    vc.energyThreshold = 0.010f;
    CVad vad(vc);

    const int64_t              base = Clock::monotonicMs();
    std::vector<SSpeechSegment> segs;
    vad.push(audio.data(), audio.size(), base, segs);
    vad.flush(segs);
    REQUIRE_FALSE(segs.empty());

    std::string        combined;
    std::vector<SWord> allWords;
    int                emitted = 0;
    for (auto& s : segs) {
        STranscript t;
        if (!Pipeline::processSegment(asr, cfg, s, EActivation::Oneshot, false, t))
            continue;
        emitted++;
        combined += " " + lower(t.text);

        // Word timestamps must be present and well-formed.
        CHECK_FALSE(t.words.empty());
        int64_t prevStart = t.onsetMs - 1000; // allow pre-roll headroom
        for (const auto& w : t.words) {
            CHECK(w.startMs <= w.endMs);           // each word start <= end
            CHECK(w.startMs >= prevStart);         // non-decreasing across words
            CHECK(w.startMs >= s.bufferStartMs - 5); // absolute (>= buffer start)
            prevStart = w.startMs;
        }
        for (auto& w : t.words)
            allWords.push_back(w);
    }

    CHECK(emitted > 0);
    REQUIRE_FALSE(allWords.empty());

    // Content check: the JFK line contains "country" (robust across model sizes).
    CHECK(combined.find("country") != std::string::npos);

    // Absolute-time sanity: first word is at/after the anchoring instant, last word
    // is within the clip duration of it.
    CHECK(allWords.front().startMs >= base - 5);
    CHECK(allWords.back().endMs <= base + (int64_t)(audio.size() * 1000 / 16000) + 1000);
}
