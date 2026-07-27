#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

// A fixed-capacity rolling buffer of the most recent capture audio.
//
// WHY (WP-V6 live-validation finding): the daemon used to open the PipeWire stream at
// PTT-press and close it at window end. Each open pays a stream connect PLUS a
// `wivrn.source` resume from SUSPENDED — which makes the WiVRn server ask the headset
// client to start its microphone over the network. That is ~0.5–1 s during which the
// user is already talking, so every transcript arrived missing its leading verb
// ("terminal to the left monitor" for "move terminal to the left monitor") and short
// utterances vanished entirely.
//
// The fix is a PERSISTENT stream plus this ring: while no capture window is open the
// daemon still holds the stream, but the frames go nowhere except here — never to the
// VAD, ASR, or wake-word tier (the privacy invariant survives: gated audio is only ever
// a sub-second transient in RAM, never analysed, never persisted). When a window opens,
// the ring is spliced onto the FRONT of the window so speech that started at or just
// before the press is still transcribed.
//
// Pure and I/O-free, so the splice semantics (retention, ordering, timestamps,
// discontinuity handling) are unit-testable with no PipeWire.
class CPreRollRing {
  public:
    CPreRollRing() = default;

    // Retain at most `ms` of `sampleRate` mono audio. ms <= 0 disables the ring
    // (push becomes a no-op) — the pre-fix behaviour, kept reachable from config.
    void configure(int sampleRate, int ms);

    // Append capture frames; the oldest samples fall out once capacity is exceeded.
    // `monoMs` is the CLOCK_MONOTONIC time of frames[0]. A gap or jump in that clock
    // (a stream restart / xrun) makes the retained audio non-contiguous, so the ring
    // drops what it holds and re-anchors rather than splice a lie into the VAD.
    void push(const float* frames, size_t n, int64_t monoMs);

    // Move the retained audio to the back of `out` and empty the ring. Returns the
    // number of samples appended; `startMonoMs` receives the time of the first one
    // (untouched when nothing was retained).
    size_t drain(std::vector<float>& out, int64_t& startMonoMs);

    void clear();

    size_t  size() const { return m_buf.size(); }
    size_t  capacity() const { return m_capacity; }
    bool    empty() const { return m_buf.empty(); }
    int     bufferedMs() const;
    // CLOCK_MONOTONIC time of the oldest retained sample (0 when empty).
    int64_t startMonoMs() const;

  private:
    std::deque<float> m_buf;
    size_t            m_capacity   = 0;
    int               m_sampleRate = 16000;

    // Sample-index bookkeeping. Times are derived from an anchor rather than advanced
    // per drop so repeated eviction cannot accumulate integer-division drift.
    int64_t m_anchorMs  = 0; // capture time of sample index m_anchorIdx
    int64_t m_anchorIdx = 0;
    int64_t m_oldestIdx = 0; // index of m_buf.front()
    int64_t m_nextIdx   = 0; // index the next pushed sample will take
};
