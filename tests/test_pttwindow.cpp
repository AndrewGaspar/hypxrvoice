#include "doctest.h"

#include "PttWindow.hpp"
#include "Vad.hpp"

#include <cmath>
#include <random>
#include <vector>

// The round-3 fixture: a Quest/WiVRn capture window. The source is DIGITALLY SILENT
// between words (measured 20th-percentile frame RMS 0.00015–0.00018 across all seven
// dumped windows) and speech peaks at only ~0.03 — an unprocessed mic with no AGC.
namespace {
    const int kSr = 16000;

    int samples(int ms) { return kSr * ms / 1000; }

    void appendSilence(std::vector<float>& b, int ms, float floorRms = 0.0002f) {
        static std::mt19937             rng(1234);
        std::normal_distribution<float> g(0.f, floorRms);
        for (int i = 0; i < samples(ms); i++)
            b.push_back(g(rng));
    }

    // A tone burst at a given RMS-ish amplitude (peak amp; RMS is amp/sqrt2).
    void appendTone(std::vector<float>& b, int ms, float amp) {
        const size_t n0 = b.size();
        for (int i = 0; i < samples(ms); i++)
            b.push_back(amp * std::sin(2.0 * M_PI * 300.0 * static_cast<double>(n0 + i) / kSr));
    }

    // The live failure shape: a short, quiet, CHOPPY word (internal stops), a gap, then
    // the rest of the phrase. Peaks ~0.03 like the real dumps.
    std::vector<float> makeQuietChoppyUtterance() {
        std::vector<float> b;
        appendSilence(b, 1000);      // the pre-roll splice: nothing said yet
        appendTone(b, 80, 0.042f);   // "fo-"
        appendSilence(b, 40);        // the stop inside the word
        appendTone(b, 80, 0.042f);   // "-cus"
        appendSilence(b, 120);
        appendTone(b, 300, 0.042f);  // "the browser"
        appendSilence(b, 900);       // the user is still holding the key
        return b;
    }

    SVadConfig liveCfg() {
        SVadConfig c; // shipped defaults: energy 0.006, start 150, gap 100, presence 100
        c.sampleRate = kSr;
        return c;
    }
}

// ---- the whole-window contract -------------------------------------------------

TEST_CASE("ptt window: the utterance is the WHOLE window, not the VAD's slice of it") {
    CPttWindow w;
    w.configure(liveCfg());
    w.begin();

    const auto    buf  = makeQuietChoppyUtterance();
    const int64_t base = 500'000;
    // Fed in 40 ms chunks, exactly like the capture callback delivers them.
    const size_t chunk = static_cast<size_t>(samples(40));
    for (size_t off = 0; off < buf.size(); off += chunk) {
        const size_t n = std::min(chunk, buf.size() - off);
        w.push(buf.data() + off, n, base + static_cast<int64_t>(off) * 1000 / kSr);
    }

    SPttUtterance u = w.finish();
    REQUIRE(u.speech);
    CHECK(u.startMs == base);
    CHECK_FALSE(u.truncated);

    // THE POINT: the buffer handed to whisper reaches back to the very start of the
    // window — the 1 s pre-roll splice included — so a leading word that never tripped
    // the VAD's onset gate is still in front of the model.
    CHECK(u.samples.size() >= static_cast<size_t>(samples(1000 + 80 + 40 + 80 + 120 + 300)));
    // …and the quiet first burst really is in there, at the place it was spoken.
    double acc = 0;
    for (int i = samples(1000); i < samples(1160); i++)
        acc += static_cast<double>(u.samples[i]) * u.samples[i];
    CHECK(std::sqrt(acc / samples(160)) > 0.005);
}

TEST_CASE("ptt window: the dead tail is trimmed but the run-out is kept") {
    CPttWindow w;
    w.configure(liveCfg());
    w.begin();

    std::vector<float> b;
    appendSilence(b, 200);
    appendTone(b, 400, 0.05f);
    appendSilence(b, 6000); // the user forgot to press the toggle again
    w.push(b.data(), b.size(), 1000);

    SPttUtterance u = w.finish();
    REQUIRE(u.speech);
    CHECK(u.trimmedTailMs > 4000);
    // The speech plus a little run-out survives; the dead seconds do not.
    const int64_t keptMs = static_cast<int64_t>(u.samples.size()) * 1000 / kSr;
    CHECK(keptMs >= 600);
    CHECK(keptMs <= 1200);
    CHECK(u.endMs == 1000 + keptMs);
}

TEST_CASE("ptt window: silence is refused without spending a whisper pass") {
    CPttWindow w;
    w.configure(liveCfg());
    w.begin();
    std::vector<float> b;
    appendSilence(b, 5000);
    w.push(b.data(), b.size(), 0);

    SPttUtterance u = w.finish();
    CHECK_FALSE(u.speech);
    CHECK(u.presence.voicedMs == 0);
}

TEST_CASE("ptt window: audio fed to the model is capped at max_utterance_ms") {
    SVadConfig cfg = liveCfg();
    cfg.maxUtteranceMs = 2000;
    CPttWindow w;
    w.configure(cfg);
    w.begin();

    std::vector<float> b;
    appendTone(b, 6000, 0.05f); // uninterrupted speech, no tail to trim
    w.push(b.data(), b.size(), 7000);

    SPttUtterance u = w.finish();
    REQUIRE(u.speech);
    CHECK(u.truncated);
    CHECK(static_cast<int64_t>(u.samples.size()) * 1000 / kSr == 2000);
    CHECK(u.endMs == 7000 + 2000);
}

TEST_CASE("ptt window: finish() with nothing captured is an empty, speechless utterance") {
    CPttWindow w;
    w.configure(liveCfg());
    SPttUtterance u = w.finish(); // never begun
    CHECK_FALSE(u.speech);
    CHECK(u.samples.empty());
    w.begin();
    u = w.finish(); // begun but never fed
    CHECK_FALSE(u.speech);
    CHECK(u.samples.empty());
}

// ---- the forgiving no-speech verdict -------------------------------------------

TEST_CASE("presence: the live 'workspace three' shape reads as speech where VAD onset did not") {
    // The dumped window that came back `no-speech`: ~400 ms of quiet, choppy speech whose
    // frames never put start_ms CONSECUTIVELY over the old 0.012 gate.
    SVadConfig cfg = liveCfg();
    std::vector<float> b;
    appendSilence(b, 1500);
    appendTone(b, 60, 0.030f);
    appendSilence(b, 60);
    appendTone(b, 120, 0.030f);
    appendSilence(b, 40);
    appendTone(b, 180, 0.030f);
    appendSilence(b, 2000);

    SSpeechPresence p = detectSpeechPresence(b.data(), b.size(), cfg);
    CHECK(p.found);
    CHECK(p.voicedMs >= cfg.presenceMs);
    CHECK(p.floorRms < 0.001f);
    CHECK(p.peakRms > 0.015f);
}

TEST_CASE("presence: the gate always sits BELOW the live VAD's own threshold") {
    // The invariant that makes this verdict safe to add: a window it rejects could never
    // have produced a segment either, so nothing that used to be transcribed stops being.
    SVadConfig cfg = liveCfg();
    std::vector<float> b;
    appendSilence(b, 500, 0.02f); // a hotter ambient
    appendTone(b, 400, 0.20f);
    appendSilence(b, 500, 0.02f);

    SSpeechPresence p = detectSpeechPresence(b.data(), b.size(), cfg);
    const float     vadThreshold = std::max(cfg.energyThreshold, p.floorRms * cfg.noiseFloorFactor);
    CHECK(p.threshold <= vadThreshold);
    CHECK(p.found);
}

TEST_CASE("presence: a single click is not speech") {
    SVadConfig cfg = liveCfg();
    std::vector<float> b;
    appendSilence(b, 1000);
    appendTone(b, 40, 0.30f); // one loud 40 ms transient — under presence_ms
    appendSilence(b, 1000);
    SSpeechPresence p = detectSpeechPresence(b.data(), b.size(), cfg);
    CHECK_FALSE(p.found);
    CHECK(p.voicedMs < cfg.presenceMs);
}
