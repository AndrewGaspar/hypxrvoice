#include "doctest.h"

#include "Vad.hpp"

#include <cmath>
#include <random>
#include <vector>

// Build a mono 16 kHz buffer: `silenceMs` of silence, then `toneMs` of a 300 Hz
// tone at the given amplitude, then trailing silence.
static std::vector<float> makeUtterance(int leadSilenceMs, int toneMs, int tailSilenceMs, float amp) {
    const int sr = 16000;
    auto      ms = [&](int m) { return sr * m / 1000; };
    std::vector<float> b;
    b.insert(b.end(), ms(leadSilenceMs), 0.f);
    int n0 = (int)b.size();
    for (int i = 0; i < ms(toneMs); i++)
        b.push_back(amp * std::sin(2.0 * M_PI * 300.0 * (n0 + i) / sr));
    b.insert(b.end(), ms(tailSilenceMs), 0.f);
    return b;
}

// The user's failure condition: a hot, AGC-driven source with a high ambient noise
// floor (~0.09 RMS). Add Gaussian noise of the given RMS to a buffer in place.
static void addNoiseFloor(std::vector<float>& b, float noiseRms, unsigned seed) {
    std::mt19937                    rng(seed);
    std::normal_distribution<float> g(0.f, noiseRms);
    for (auto& x : b)
        x += g(rng);
}

TEST_CASE("vad: segments a single tone burst with a plausible onset") {
    SVadConfig cfg;
    cfg.energyThreshold = 0.05f;
    cfg.startMs         = 100;
    cfg.endMs           = 300;
    cfg.preRollMs       = 200;
    CVad vad(cfg);

    // 500 ms silence, 800 ms tone, 600 ms silence.
    auto buf = makeUtterance(500, 800, 600, 0.3f);

    const int64_t              base = 1'000'000; // arbitrary monotonic base
    std::vector<SSpeechSegment> segs;
    vad.push(buf.data(), buf.size(), base, segs);
    vad.flush(segs);

    REQUIRE(segs.size() == 1);
    const auto& s = segs[0];
    // Onset should land near 500 ms after base (within a few frames + startMs).
    CHECK(s.onsetMs >= base + 400);
    CHECK(s.onsetMs <= base + 700);
    CHECK(s.endMs > s.onsetMs);
    CHECK(s.bufferStartMs <= s.onsetMs);          // pre-roll precedes onset
    CHECK(s.bufferStartMs >= base);               // but not before the stream
    CHECK(s.samples.size() > (size_t)(16000 * 0.5)); // at least ~the tone
}

TEST_CASE("vad: pure silence produces no segment") {
    SVadConfig cfg;
    cfg.energyThreshold = 0.05f;
    CVad vad(cfg);
    std::vector<float>          sil(16000, 0.f);
    std::vector<SSpeechSegment> segs;
    vad.push(sil.data(), sil.size(), 0, segs);
    vad.flush(segs);
    CHECK(segs.empty());
}

// ---- noise-floor-adaptive VAD (the live-capture bug #1 fix) ----

TEST_CASE("vad: FIXED threshold over a hot noise floor never endpoints (the bug)") {
    // Reproduces the live failure: energy_threshold 0.012 with an ambient floor of
    // ~0.09 makes every frame "voiced", so the utterance never sees trailing silence
    // and runs to max_utterance_ms — one giant segment of noise, not a crisp utterance.
    SVadConfig cfg;
    cfg.adaptive       = false; // OLD behaviour
    cfg.energyThreshold = 0.012f;
    cfg.startMs        = 150;
    cfg.endMs          = 600;
    cfg.maxUtteranceMs = 2000; // keep the test fast
    // 500 ms lead, 400 ms tone, 2000 ms tail — with a hot floor the tail never reads silent.
    auto buf = makeUtterance(500, 400, 2000, 0.30f);
    addNoiseFloor(buf, 0.09f, 1);

    CVad vad(cfg);
    std::vector<SSpeechSegment> segs;
    vad.push(buf.data(), buf.size(), 0, segs);
    // The only way it closes is by hitting max_utterance_ms; it can NEVER endpoint on
    // silence. So any emitted segment spans ~the whole max window (noise), never the
    // 400 ms tone.
    for (const auto& s : segs)
        CHECK((s.endMs - s.onsetMs) >= cfg.maxUtteranceMs - cfg.frameMs);
}

TEST_CASE("vad: ADAPTIVE gating endpoints a real utterance over a hot noise floor") {
    SVadConfig cfg;
    cfg.adaptive        = true;
    cfg.energyThreshold = 0.012f;
    cfg.noiseFloorFactor = 1.6f;
    cfg.startMs         = 150;
    cfg.endMs           = 500;
    cfg.maxUtteranceMs  = 12000;
    // 800 ms ambient lead (lets the floor calibrate), 700 ms loud tone, 900 ms tail.
    auto buf = makeUtterance(800, 700, 900, 0.30f);
    addNoiseFloor(buf, 0.09f, 7);

    CVad vad(cfg);
    std::vector<SSpeechSegment> segs;
    vad.push(buf.data(), buf.size(), 0, segs);
    vad.flush(segs);

    // The floor must have converged toward the real ambient (~0.09), lifting the gate
    // well above the fixed 0.012.
    CHECK(vad.noiseFloor() > 0.05f);
    CHECK(vad.threshold() > 0.09f);
    // Exactly one endpointed utterance, and it CLOSED on silence (not the 12 s cap).
    REQUIRE(segs.size() == 1);
    CHECK((segs[0].endMs - segs[0].onsetMs) < 3000);
    // Onset lands in the tone region (~800 ms), not at t=0 in the noise lead.
    CHECK(segs[0].onsetMs >= 600);
    CHECK(segs[0].onsetMs <= 1800);
}

TEST_CASE("vad: adaptive floor still catches quiet speech on a quiet source") {
    // A near-silent source (floor ~0.003) must not be over-gated: the energy_threshold
    // is the minimum, so a modest tone above it still segments.
    SVadConfig cfg;
    cfg.adaptive        = true;
    cfg.energyThreshold = 0.012f;
    cfg.noiseFloorFactor = 1.6f;
    cfg.startMs         = 100;
    cfg.endMs           = 400;
    auto buf = makeUtterance(600, 500, 600, 0.10f);
    addNoiseFloor(buf, 0.003f, 3);

    CVad vad(cfg);
    std::vector<SSpeechSegment> segs;
    vad.push(buf.data(), buf.size(), 0, segs);
    vad.flush(segs);
    REQUIRE(segs.size() == 1);
    CHECK((segs[0].endMs - segs[0].onsetMs) < 2000);
}

TEST_CASE("vad: contiguous chunked feeding matches timing") {
    SVadConfig cfg;
    cfg.energyThreshold = 0.05f;
    cfg.startMs         = 100;
    cfg.endMs           = 300;
    CVad vad(cfg);
    auto buf = makeUtterance(300, 600, 500, 0.3f);

    const int64_t              base = 500'000;
    std::vector<SSpeechSegment> segs;
    // Feed in 40 ms chunks with monotonically advancing timestamps.
    const size_t chunk = 16000 * 40 / 1000;
    for (size_t off = 0; off < buf.size(); off += chunk) {
        size_t  n  = std::min(chunk, buf.size() - off);
        int64_t ts = base + (int64_t)(off * 1000 / 16000);
        vad.push(buf.data() + off, n, ts, segs);
    }
    vad.flush(segs);
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].onsetMs >= base + 200);
    CHECK(segs[0].onsetMs <= base + 500);
}
