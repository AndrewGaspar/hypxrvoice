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

// ---- onset back-pad (the round-2 "leading word is missing" fix) ----
//
// The ring is fed EVERY idle frame, including the voiced frames of the run that declares
// onset, and onset is back-dated to the FIRST of those. So a ring of exactly pre_roll_ms
// leaves only (pre_roll_ms - start_ms) in front of the ONSET instant. A first syllable
// that is quiet — an unvoiced fricative, or any source still ramping its gain when you
// start talking — lives in that gap and was dropped even though it was in the buffer.

// A quiet leading syllable (below the gate) followed by the loud body of the utterance.
// Layout: 600 ms silence | 300 ms quiet | 700 ms loud | 600 ms silence.
static std::vector<float> makeQuietOnset(float quietAmp, float loudAmp) {
    const int sr = 16000;
    auto      ms = [&](int m) { return sr * m / 1000; };
    std::vector<float> b;
    b.insert(b.end(), ms(600), 0.f);
    for (int i = 0; i < ms(300); i++)
        b.push_back(quietAmp * std::sin(2.0 * M_PI * 300.0 * (b.size() + i) / sr));
    for (int i = 0; i < ms(700); i++)
        b.push_back(loudAmp * std::sin(2.0 * M_PI * 300.0 * (b.size() + i) / sr));
    b.insert(b.end(), ms(600), 0.f);
    return b;
}

TEST_CASE("vad: a quiet first syllable survives — the segment carries the full back-pad") {
    SVadConfig cfg;
    cfg.adaptive        = false; // deterministic gate; the adaptive path has its own tests
    cfg.energyThreshold = 0.05f;
    cfg.startMs         = 100;
    cfg.endMs           = 300;
    cfg.preRollMs       = 300;
    cfg.onsetBackpadMs  = 300;
    CVad vad(cfg);

    // Quiet amp 0.05 => RMS ~0.035, comfortably UNDER the 0.05 gate, so the syllable
    // never triggers onset by itself. It must be recovered by the back-pad, not the gate.
    auto buf = makeQuietOnset(0.05f, 0.30f);

    std::vector<SSpeechSegment> segs;
    vad.push(buf.data(), buf.size(), 0, segs);
    vad.flush(segs);
    REQUIRE(segs.size() == 1);
    const auto& s = segs[0];

    // Onset lands on the LOUD body (~900 ms), which is the only thing that trips the gate.
    CHECK(s.onsetMs >= 850);
    CHECK(s.onsetMs <= 1000);
    // …and the contract: at least onset_backpad_ms of audio sits in front of it, which is
    // enough to reach back to the start of the quiet syllable at 600 ms.
    CHECK(s.backpadMs() >= cfg.onsetBackpadMs);
    CHECK(s.bufferStartMs <= 600);

    // The quiet syllable is really in the samples — not silence, not a hole.
    const size_t quietEnd = static_cast<size_t>((900 - s.bufferStartMs) * 16) ;
    REQUIRE(quietEnd > 0);
    REQUIRE(quietEnd <= s.samples.size());
    double acc = 0;
    for (size_t i = 0; i < quietEnd; i++)
        acc += static_cast<double>(s.samples[i]) * s.samples[i];
    const double leadRms = std::sqrt(acc / quietEnd);
    CHECK(leadRms > 0.02); // ~0.035 for a 0.05-amplitude tone; nowhere near silence
}

TEST_CASE("vad: with no back-pad the same quiet syllable is clipped (the bug)") {
    // onset_backpad_ms = 0 reproduces the old sizing: the ring is just pre_roll_ms, so
    // start_ms of it is spent on the voiced run and the segment begins INSIDE the quiet
    // syllable. Same audio, same gate — only the retention changes.
    SVadConfig cfg;
    cfg.adaptive        = false;
    cfg.energyThreshold = 0.05f;
    cfg.startMs         = 100;
    cfg.endMs           = 300;
    cfg.preRollMs       = 300;
    cfg.onsetBackpadMs  = 0;
    CVad vad(cfg);

    auto                        buf = makeQuietOnset(0.05f, 0.30f);
    std::vector<SSpeechSegment> segs;
    vad.push(buf.data(), buf.size(), 0, segs);
    vad.flush(segs);
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].backpadMs() < 300);
    CHECK(segs[0].bufferStartMs > 600); // the syllable started at 600 ms and got cut
}

TEST_CASE("vad: the splice does not duplicate the trigger frame") {
    // The utterance used to be seeded with the whole ring AND the current frame again —
    // 20 ms of duplicated audio that shifted every ASR word timestamp after the splice.
    // Sample count vs. the timestamps is the check: samples must span exactly
    // bufferStartMs..endMs with no extra frame.
    SVadConfig cfg;
    cfg.adaptive        = false;
    cfg.energyThreshold = 0.05f;
    cfg.startMs         = 100;
    cfg.endMs           = 300;
    cfg.preRollMs       = 200;
    cfg.onsetBackpadMs  = 200;
    CVad vad(cfg);

    auto                        buf = makeUtterance(500, 800, 600, 0.3f);
    std::vector<SSpeechSegment> segs;
    vad.push(buf.data(), buf.size(), 0, segs);
    REQUIRE(segs.size() == 1);
    const auto&   s        = segs[0];
    const int64_t spanMs   = s.endMs - s.bufferStartMs;
    const int64_t actualMs = static_cast<int64_t>(s.samples.size()) * 1000 / 16000;
    CHECK(actualMs == spanMs);
}

TEST_CASE("vad: a second utterance right after the first still gets its full back-pad") {
    // The ring used to be fed only while IDLE, so it was emptied into the utterance at
    // onset and stayed empty until that utterance endpointed. Speak again shortly after
    // the hangover and the back-pad was whatever had refilled since — near zero — which
    // reopened the very gap this feature closes, just for the second command.
    SVadConfig cfg;
    cfg.adaptive        = false;
    cfg.energyThreshold = 0.05f;
    cfg.startMs         = 100;
    cfg.endMs           = 300;
    cfg.preRollMs       = 300;
    cfg.onsetBackpadMs  = 300;
    CVad vad(cfg);

    // 600 silence | 400 loud | 350 silence (endpoints at 1300) | 150 quiet | 700 loud | tail
    const int sr = 16000;
    auto      ms = [&](int m) { return sr * m / 1000; };
    auto      tone = [&](std::vector<float>& b, int lenMs, float amp) {
        const size_t n0 = b.size();
        for (int i = 0; i < ms(lenMs); i++)
            b.push_back(amp * std::sin(2.0 * M_PI * 300.0 * (n0 + i) / sr));
    };
    std::vector<float> buf;
    buf.insert(buf.end(), ms(600), 0.f);
    tone(buf, 400, 0.30f);
    buf.insert(buf.end(), ms(350), 0.f);
    tone(buf, 150, 0.05f); // the second command's quiet first syllable
    tone(buf, 700, 0.30f);
    buf.insert(buf.end(), ms(600), 0.f);

    std::vector<SSpeechSegment> segs;
    vad.push(buf.data(), buf.size(), 0, segs);
    vad.flush(segs);
    REQUIRE(segs.size() == 2);
    CHECK(segs[0].backpadMs() >= cfg.onsetBackpadMs);
    CHECK(segs[1].backpadMs() >= cfg.onsetBackpadMs);
    // …and that back-pad reaches back past the start of the quiet syllable (1350 ms).
    CHECK(segs[1].bufferStartMs <= 1350);
}

// ---- gap-tolerant onset (the round-3 "choppy word never triggers" fix) ----
//
// start_ms used to demand start_ms of CONSECUTIVE voiced frames. No real word delivers
// that: "focus" has a stop gap before its /k/, "workspace" one before its /sp/. Each dip
// reset the run to zero, so a short quiet word could never declare onset at all — the
// live dumps show "focus the browser" onsetting at 2.82 s ("browser") with "focus" plainly
// present at 2.04 s, and "workspace three" producing no segment whatsoever.

// A choppy word: `voicedMs` of tone, a `gapMs` hole, then `voicedMs` of tone again.
static std::vector<float> makeChoppyWord(int leadMs, int voicedMs, int gapMs, int tailMs, float amp) {
    const int sr = 16000;
    auto      ms = [&](int m) { return sr * m / 1000; };
    std::vector<float> b;
    auto tone = [&](int lenMs) {
        const size_t n0 = b.size();
        for (int i = 0; i < ms(lenMs); i++)
            b.push_back(amp * std::sin(2.0 * M_PI * 300.0 * (n0 + i) / sr));
    };
    b.insert(b.end(), ms(leadMs), 0.f);
    tone(voicedMs);
    b.insert(b.end(), ms(gapMs), 0.f);
    tone(voicedMs);
    b.insert(b.end(), ms(tailMs), 0.f);
    return b;
}

TEST_CASE("vad: a choppy word (80ms / 40ms gap / 80ms) declares onset") {
    SVadConfig cfg;
    cfg.adaptive        = false; // deterministic gate
    cfg.energyThreshold = 0.05f;
    cfg.startMs         = 150;
    cfg.endMs           = 300;
    cfg.gapToleranceMs  = 100;
    CVad vad(cfg);

    auto                        buf = makeChoppyWord(600, 80, 40, 800, 0.30f);
    std::vector<SSpeechSegment> segs;
    vad.push(buf.data(), buf.size(), 0, segs);
    vad.flush(segs);

    REQUIRE(segs.size() == 1);
    // 160 ms of VOICED frames across one 40 ms dip is enough — and the onset is back-dated
    // to the FIRST voiced frame, i.e. the true start of the word at 600 ms.
    CHECK(segs[0].onsetMs >= 580);
    CHECK(segs[0].onsetMs <= 640);
}

TEST_CASE("vad: with no gap tolerance the same word is invisible (the bug)") {
    SVadConfig cfg;
    cfg.adaptive        = false;
    cfg.energyThreshold = 0.05f;
    cfg.startMs         = 150;
    cfg.endMs           = 300;
    cfg.gapToleranceMs  = 0; // the old behaviour
    CVad vad(cfg);

    auto                        buf = makeChoppyWord(600, 80, 40, 800, 0.30f);
    std::vector<SSpeechSegment> segs;
    vad.push(buf.data(), buf.size(), 0, segs);
    vad.flush(segs);
    CHECK(segs.empty()); // neither 80 ms half reaches start_ms on its own
}

TEST_CASE("vad: the gap tolerance does not count toward start_ms") {
    // The tolerance PAUSES the run; it must not fill it. 60 ms of voicing either side of a
    // 60 ms hole is 120 ms of speech — still short of a 150 ms start_ms — so nothing fires.
    // This is what keeps the gate no looser on noise than it was.
    SVadConfig cfg;
    cfg.adaptive        = false;
    cfg.energyThreshold = 0.05f;
    cfg.startMs         = 150;
    cfg.endMs           = 300;
    cfg.gapToleranceMs  = 100;
    CVad vad(cfg);

    auto                        buf = makeChoppyWord(600, 60, 60, 800, 0.30f);
    std::vector<SSpeechSegment> segs;
    vad.push(buf.data(), buf.size(), 0, segs);
    vad.flush(segs);
    CHECK(segs.empty());
}

TEST_CASE("vad: a gap LONGER than the tolerance still resets the run") {
    SVadConfig cfg;
    cfg.adaptive        = false;
    cfg.energyThreshold = 0.05f;
    cfg.startMs         = 150;
    cfg.endMs           = 300;
    cfg.gapToleranceMs  = 100;
    CVad vad(cfg);

    // 120 ms voiced, a 200 ms hole (over the tolerance), 120 ms voiced: two runs of 120 ms,
    // neither of which reaches start_ms.
    auto                        buf = makeChoppyWord(600, 120, 200, 800, 0.30f);
    std::vector<SSpeechSegment> segs;
    vad.push(buf.data(), buf.size(), 0, segs);
    vad.flush(segs);
    CHECK(segs.empty());
}

TEST_CASE("vad: the back-pad guarantee survives a gap-spanning onset run") {
    // The ring is sized for start_ms + gap_tolerance_ms of run, so the onsetBackpadMs
    // contract holds even when the run that declared onset spanned a dip.
    SVadConfig cfg;
    cfg.adaptive        = false;
    cfg.energyThreshold = 0.05f;
    cfg.startMs         = 150;
    cfg.endMs           = 300;
    cfg.gapToleranceMs  = 100;
    cfg.preRollMs       = 300;
    cfg.onsetBackpadMs  = 300;
    CVad vad(cfg);

    auto                        buf = makeChoppyWord(600, 100, 80, 800, 0.30f);
    std::vector<SSpeechSegment> segs;
    vad.push(buf.data(), buf.size(), 0, segs);
    vad.flush(segs);
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].backpadMs() >= cfg.onsetBackpadMs);
    CHECK(segs[0].bufferStartMs <= 300);
}
