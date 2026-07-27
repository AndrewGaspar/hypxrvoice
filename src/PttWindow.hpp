#pragma once

#include "Vad.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// The audio of ONE push-to-talk capture window, kept whole.
//
// WHY (round-3 design decision, from the live capture dumps): a PTT press is an EXPLICIT
// declaration that speech is coming. Gating what reaches whisper on VAD ONSET therefore
// buys nothing and costs everything — the dumps show two separate failures of exactly
// that gate in one six-utterance round:
//
//   * "focus the browser" — "focus" is plainly in the window wav at 2.04 s, but the
//     onset run kept resetting on the word's internal stop, so the segment began at
//     2.52 s and whisper was handed "The browser."
//   * "workspace three"   — ~400 ms of real speech in the wav, no segment at all,
//     outcome `no-speech`, because the whole word never put start_ms of CONSECUTIVE
//     frames over the energy gate.
//
// So for a PTT window the utterance IS the window: the pre-roll splice through the
// release/timeout, minus an obviously-empty tail, capped at max_utterance_ms. Whisper
// copes with internal silence perfectly well; it is the missing leading verb that makes
// a command unparseable. The VAD stays in the loop for the two things it is still good
// at — early endpointing (so a short utterance need not wait for the PTT deadline) and a
// FORGIVING whole-window no-speech verdict (detectSpeechPresence) that only exists to
// keep several seconds of pure silence away from the model.
//
// Pure and I/O-free — no PipeWire, no whisper, no clock of its own — so the splice,
// trimming, capping and presence rules are all directly unit-testable.

struct SPttUtterance {
    // The forgiving verdict: is this worth transcribing at all? When false the daemon
    // reports `no-speech` and never calls the ASR.
    bool               speech = false;
    std::vector<float> samples;        // 16 kHz mono: the whole window, tail-trimmed
    int64_t            startMs  = 0;   // CLOCK_MONOTONIC of samples[0]
    int64_t            endMs    = 0;   // CLOCK_MONOTONIC just past samples.back()
    bool               truncated = false; // hit the max_utterance_ms cap
    int64_t            trimmedTailMs = 0; // silence dropped off the end
    SSpeechPresence    presence;       // the numbers behind `speech`, for logs/dumps
};

class CPttWindow {
  public:
    // `vad` supplies the sample rate, frame size, the energy floor the presence verdict
    // scales off, max_utterance_ms (the cap on audio fed to whisper) and presence_ms.
    void configure(const SVadConfig& vad);

    // Open a window. Any previously accumulated audio is discarded.
    void begin();

    // Append capture frames in arrival order. `monoMs` is the CLOCK_MONOTONIC time of
    // frames[0]; the first call anchors the window clock. A jump in that clock (stream
    // restart / xrun) is noted and the audio is concatenated anyway — the alternative
    // is throwing away the user's words over a few milliseconds of drift.
    void push(const float* frames, size_t n, int64_t monoMs);

    bool    open() const { return m_open; }
    size_t  size() const { return m_buf.size(); }
    int64_t durationMs() const;

    // Close the window and yield its utterance. Safe to call with no window open (the
    // result is an empty, speech=false utterance).
    SPttUtterance finish();

  private:
    SVadConfig         m_cfg;
    bool               m_open       = false;
    bool               m_haveStart  = false;
    int64_t            m_startMs    = 0;
    int64_t            m_nextMs     = 0; // where the capture clock is expected next
    bool               m_gapSeen    = false;
    std::vector<float> m_buf;
};
