#include "doctest.h"

#include "Vad.hpp"

#include <cmath>
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
