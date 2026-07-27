#include "doctest.h"

#include "PreRoll.hpp"
#include "Vad.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

// WP-V6 bug #1 — the leading words of every PTT utterance were lost because the capture
// stream was opened at the key press and `wivrn.source` needed ~0.5–1 s to resume (the
// WiVRn server has to ask the headset client to start its mic over the network). The fix
// holds the stream open and keeps a rolling pre-roll ring of the gated audio, splicing it
// onto the front of the next window.
//
// These tests cover the ring's retention/ordering/timestamp contract and then the thing
// that actually matters: with the splice, a word that STARTED BEFORE the press survives
// into the VAD segment; without it, the same audio loses its onset.

namespace {
    constexpr int kSr = 16000;

    size_t samplesFor(int ms) { return static_cast<size_t>(kSr) * static_cast<size_t>(ms) / 1000; }

    // `ms` of a 300 Hz tone at `amp` (0 => silence), phase-continuous from `phase`.
    std::vector<float> tone(int ms, float amp, size_t phase = 0) {
        std::vector<float> b(samplesFor(ms));
        for (size_t i = 0; i < b.size(); i++)
            b[i] = amp == 0.f ? 0.f
                              : amp * std::sin(2.0 * M_PI * 300.0 * static_cast<double>(phase + i) / kSr);
        return b;
    }

    SVadConfig vadCfg() {
        SVadConfig c;
        c.sampleRate      = kSr;
        c.adaptive        = false; // deterministic gating; the adaptive path has its own tests
        c.energyThreshold = 0.05f;
        c.startMs         = 100;
        c.endMs           = 300;
        c.preRollMs       = 200;
        return c;
    }
}

// ---- the ring itself ------------------------------------------------------------------

TEST_CASE("preroll: retains only the most recent window, oldest-first") {
    CPreRollRing ring;
    ring.configure(kSr, 500);
    CHECK(ring.capacity() == samplesFor(500));

    // Push 1200 ms of a ramp so each sample is identifiable by value.
    std::vector<float> ramp(samplesFor(1200));
    for (size_t i = 0; i < ramp.size(); i++)
        ramp[i] = static_cast<float>(i);

    const int64_t base  = 900'000;
    const size_t  chunk = samplesFor(20);
    for (size_t off = 0; off < ramp.size(); off += chunk)
        ring.push(ramp.data() + off, std::min(chunk, ramp.size() - off),
                  base + static_cast<int64_t>(off * 1000 / kSr));

    CHECK(ring.size() == samplesFor(500));
    CHECK(ring.bufferedMs() == 500);

    std::vector<float> out;
    int64_t            startMs = 0;
    const size_t       n       = ring.drain(out, startMs);
    REQUIRE(n == samplesFor(500));
    // Oldest retained sample is the one 500 ms from the end, and order is preserved.
    CHECK(out.front() == doctest::Approx(static_cast<float>(ramp.size() - samplesFor(500))));
    CHECK(out.back() == doctest::Approx(static_cast<float>(ramp.size() - 1)));
    // ...and its timestamp is the capture time of THAT sample, not of the first push.
    CHECK(startMs == base + 700); // 1200 ms pushed - 500 ms retained
    // Draining empties the ring.
    CHECK(ring.empty());
    CHECK(ring.bufferedMs() == 0);
}

TEST_CASE("preroll: drain of an empty ring reports nothing and leaves the sink alone") {
    CPreRollRing ring;
    ring.configure(kSr, 800);
    std::vector<float> out{1.f, 2.f};
    int64_t            startMs = 4242;
    CHECK(ring.drain(out, startMs) == 0);
    CHECK(out.size() == 2);      // untouched
    CHECK(startMs == 4242);      // untouched
}

TEST_CASE("preroll: zero capacity disables the ring entirely") {
    CPreRollRing ring;
    ring.configure(kSr, 0);
    auto buf = tone(300, 0.3f);
    ring.push(buf.data(), buf.size(), 1000);
    CHECK(ring.size() == 0);
    std::vector<float> out;
    int64_t            startMs = 0;
    CHECK(ring.drain(out, startMs) == 0);
}

TEST_CASE("preroll: a capture-clock discontinuity drops the stale audio") {
    // A stream restart (source vanished and came back) leaves the ring holding audio that
    // is NOT contiguous with what follows. Splicing that into the VAD would fabricate an
    // utterance out of two unrelated instants, so the ring re-anchors instead.
    CPreRollRing ring;
    ring.configure(kSr, 1000);

    auto a = tone(400, 0.3f);
    ring.push(a.data(), a.size(), 10'000);
    REQUIRE(ring.size() == a.size());

    auto b = tone(200, 0.3f);
    ring.push(b.data(), b.size(), 10'000 + 400 + 5000); // 5 s gap: a restart
    CHECK(ring.size() == b.size());                     // only the post-gap audio survives
    CHECK(ring.startMonoMs() == 10'000 + 400 + 5000);

    // Ordinary buffer jitter (a few ms) must NOT trip it.
    auto c = tone(200, 0.3f);
    ring.push(c.data(), c.size(), 10'000 + 400 + 5000 + 200 + 8);
    CHECK(ring.size() == b.size() + c.size());
}

// ---- the splice: what the fix is actually for -------------------------------------------

// Feed `gated` into the ring, then open a window (fresh VAD seeded with the ring) and feed
// `live`. Mirrors CDaemon::openCaptureWindow + drainAudio exactly.
static std::vector<SSpeechSegment> runWindow(int preRollMs, const std::vector<float>& gated,
                                             const std::vector<float>& live, int64_t base) {
    CPreRollRing ring;
    ring.configure(kSr, preRollMs);
    ring.push(gated.data(), gated.size(), base);

    CVad                        vad(vadCfg());
    std::vector<SSpeechSegment> segs;

    std::vector<float> pre;
    int64_t            preStartMs = 0;
    if (ring.drain(pre, preStartMs) > 0)
        vad.push(pre.data(), pre.size(), preStartMs, segs);

    const int64_t liveStart = base + static_cast<int64_t>(gated.size() * 1000 / kSr);
    vad.push(live.data(), live.size(), liveStart, segs);
    vad.flush(segs);
    return segs;
}

TEST_CASE("preroll: speech that began BEFORE the window opens survives the splice") {
    const int64_t base = 500'000;
    // The live sequence: the user starts the sentence, and only ~700 ms later does the
    // capture window open. 300 ms of room tone, then 700 ms of speech, all while gated.
    std::vector<float> gated = tone(300, 0.f);
    {
        auto speech = tone(700, 0.3f, samplesFor(300));
        gated.insert(gated.end(), speech.begin(), speech.end());
    }
    // Then the window opens and the sentence continues for another 500 ms, then stops.
    std::vector<float> live = tone(500, 0.3f, samplesFor(1000));
    {
        auto tail = tone(600, 0.f);
        live.insert(live.end(), tail.begin(), tail.end());
    }
    const int64_t windowOpenMs = base + 1000;

    SUBCASE("with a 1 s pre-roll the onset predates the window") {
        auto segs = runWindow(1000, gated, live, base);
        REQUIRE(segs.size() == 1);
        // Onset is back in the GATED region (~base+300), not at the window open instant.
        CHECK(segs[0].onsetMs < windowOpenMs);
        CHECK(segs[0].onsetMs >= base + 200);
        CHECK(segs[0].bufferStartMs <= segs[0].onsetMs);
        // ...and the captured audio contains the pre-window speech (700 ms of it) plus
        // the 500 ms that followed.
        CHECK(segs[0].samples.size() >= samplesFor(1100));
    }

    SUBCASE("without the pre-roll the leading speech is lost (the reported bug)") {
        auto segs = runWindow(0, gated, live, base);
        REQUIRE(segs.size() == 1);
        // Everything before the window open instant is gone: the onset can only be at or
        // after it, and the segment is barely longer than the post-open remainder.
        CHECK(segs[0].onsetMs >= windowOpenMs);
        CHECK(segs[0].samples.size() < samplesFor(1100));
    }
}

TEST_CASE("preroll: a short utterance entirely inside the pre-roll is not lost") {
    // The 22:25:58 / 22:26:01 case — "workspace three" said and finished while the stream
    // was still coming up. With the splice it is captured; the window itself is silent.
    const int64_t      base  = 700'000;
    std::vector<float> gated = tone(200, 0.f);
    {
        auto speech = tone(500, 0.3f, samplesFor(200));
        gated.insert(gated.end(), speech.begin(), speech.end());
        auto tail = tone(400, 0.f);
        gated.insert(gated.end(), tail.begin(), tail.end());
    }
    std::vector<float> live = tone(400, 0.f); // nothing more is said after the press

    auto with = runWindow(1500, gated, live, base);
    REQUIRE(with.size() == 1);
    CHECK((with[0].endMs - with[0].onsetMs) >= 400);

    auto without = runWindow(0, gated, live, base);
    CHECK(without.empty()); // exactly the "capture started/stopped, no transcript" symptom
}
