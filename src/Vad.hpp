#pragma once

#include <cstdint>
#include <deque>
#include <vector>

// Energy-gated voice-activity detector / utterance segmenter. Cheap enough to run
// continuously while ARMED (near-zero CPU on silence — just an RMS per 20 ms frame),
// which is what keeps the idle cost low. It produces speech segments carrying the
// CLOCK_MONOTONIC onset timestamp that anchors the whole transcript.
//
// NOTE (deviation from the research doc, justified in the report): the doc names
// silero-VAD. Silero requires an ONNX runtime — a heavy vendored/system dependency
// that conflicts with the "no system installs" constraint. Energy VAD with a
// start/hangover hysteresis is fully local, dependency-free, and testable; the
// interface below is the seam where a silero backend would drop in later.
struct SVadConfig {
    int   sampleRate      = 16000;
    // RMS floor: a frame below this is ALWAYS silence.
    //
    // WHY 0.006 AND NOT 0.012 (round-3 live measurement): all seven capture windows
    // dumped from a Quest 3 / WiVRn session have a 20th-percentile frame RMS of
    // 0.00015–0.00018 (the source is digitally silent between words) and speech PEAKS of
    // only 0.029–0.042 — an unprocessed headset mic with no AGC. At 0.012 the utterance
    // "workspace three" put just 140 ms of frames over the gate in TOTAL, less than the
    // 150 ms start_ms run, so onset never fired and the window was scored `no-speech`
    // while the wav plainly contains the words. 0.006 is still ~30x that measured floor,
    // and the adaptive term below is what actually protects a hot source.
    float energyThreshold = 0.006f;
    int   startMs         = 150;    // sustained voiced to declare onset
    int   endMs           = 600;    // sustained silence (hangover) to declare end
    int   maxUtteranceMs  = 12000;
    int   preRollMs       = 300;    // raw depth of the idle retention ring
    int   frameMs         = 20;     // analysis window

    // How long an unvoiced DIP inside the onset run may last before the run is thrown
    // away (round-3 live finding). startMs used to demand startMs of CONSECUTIVE voiced
    // frames, which no real word delivers: "focus" has a stop gap before the /k/ and
    // "workspace" one before the /sp/, so each dip reset the counter to zero and the
    // detector waited for a later, louder word — the dumped window for "focus the
    // browser" declared onset at 2.82 s ("browser") when "focus" had been sitting at
    // 2.04 s. With a tolerance the run PAUSES across a short gap instead of resetting.
    // Only voiced frames count toward startMs, so the gate is no looser on noise (a lone
    // spike still needs startMs of real voicing) — but a choppy word finally survives.
    // Replaying that live wav through the detector, 100 ms moves onset 2.82 s -> 2.04 s.
    int   gapToleranceMs  = 100;

    // How much energetic audio ANYWHERE in a buffer counts as "somebody spoke", for the
    // whole-buffer verdict used by the PTT path (see detectSpeechPresence). This is not
    // an onset criterion — it need not be contiguous and it deliberately gates lower than
    // the VAD itself. Its only job is to keep several seconds of pure silence away from
    // whisper; anything that might be speech is transcribed.
    int   presenceMs      = 100;

    // Audio GUARANTEED to sit in front of the DECLARED ONSET instant.
    //
    // WHY THIS IS NOT preRollMs (round-2 live finding): the ring is fed EVERY idle
    // frame, including the voiced frames of the run that eventually declares onset, and
    // onset is back-dated to the FIRST voiced frame of that run (startMs earlier). So a
    // ring of exactly preRollMs leaves only (preRollMs - startMs) in front of onset —
    // 150 ms at the shipped defaults, half of what preRollMs's own comment claimed. A
    // first syllable that is quiet (an unvoiced fricative; or any source whose AGC is
    // still ramping when you start talking) sits under the gate for longer than that and
    // was structurally dropped even though it had been sitting in the ring the whole
    // time. The ring is therefore sized as max(preRollMs, onsetBackpadMs + startMs), and
    // an emitted segment always carries at least onsetBackpadMs of audio before onsetMs
    // — clamped, of course, to the audio the stream has actually delivered so far.
    int   onsetBackpadMs  = 300;

    // Noise-floor-adaptive gating. A FIXED energyThreshold cannot survive a hot,
    // AGC-driven source (e.g. the Quest/WiVRn mic, whose ambient RMS ran ~0.09 —
    // far above 0.012): every frame reads "voiced", the utterance never sees the
    // 600 ms of trailing silence needed to endpoint, and it runs to maxUtteranceMs
    // transcribing noise (presenting as "listening… no response"). With `adaptive`,
    // the detector estimates the noise floor as a low percentile of the RMS over a
    // rolling window and gates at max(energyThreshold, floor * noiseFloorFactor): a
    // low percentile tracks the ambient/pauses (persistent) without being dragged up
    // by transient speech bursts, so onset needs speech ABOVE the ambient and pauses
    // fall back below it to endpoint. Converges within the window with no warmup, so
    // onset timing on a quiet source is unchanged.
    bool  adaptive        = true;
    float noiseFloorFactor = 1.6f;  // voiced threshold = floor * this (>= energyThreshold)
    int   noiseWindowMs   = 1500;   // rolling window for the noise-floor percentile
};

struct SSpeechSegment {
    int64_t            onsetMs       = 0; // CLOCK_MONOTONIC ms of speech onset
    int64_t            endMs         = 0; // CLOCK_MONOTONIC ms of speech end
    int64_t            bufferStartMs = 0; // CLOCK_MONOTONIC ms of samples[0]
                                          // (= onset minus captured pre-roll). ASR
                                          // word offsets are added to THIS.
    std::vector<float> samples;           // 16 kHz mono, incl. pre-roll

    // How much audio actually landed in FRONT of the onset instant. Equals
    // onsetMs - bufferStartMs; carried explicitly because it is the number that says
    // whether a quiet first syllable could have survived (see SVadConfig::onsetBackpadMs)
    // and it is what the debug dumps and the daemon log line report.
    int64_t backpadMs() const { return onsetMs - bufferStartMs; }
};

// The whole-buffer "did anybody say anything?" verdict.
//
// WHY IT EXISTS (round-3 design decision): a PTT press is an EXPLICIT declaration that
// speech is coming, so the PTT path no longer asks the VAD which slice of the window to
// transcribe — it hands whisper the whole window (CPttWindow). The only thing still worth
// deciding is whether the window is worth transcribing AT ALL, and that decision must be
// far more forgiving than onset detection: it scans the entire buffer, needs no
// contiguity, and gates at HALF the fixed energy floor (or 1.5x the buffer's own noise
// floor, whichever is larger — always <= the VAD's own threshold, by construction). If it
// is wrong, whisper returns junk and the intent tier rejects it; that is a fine outcome.
// Silently eating speech is not.
struct SSpeechPresence {
    bool  found     = false;
    int   voicedMs  = 0; // total ms of frames over `threshold`, anywhere in the buffer
    float floorRms  = 0.f; // 20th-percentile frame RMS (the buffer's own ambient)
    float peakRms   = 0.f; // loudest frame RMS
    float threshold = 0.f; // the gate that produced `voicedMs`
};

// Scan `n` mono samples for speech presence per the rules above. Pure — no state, no
// allocation beyond one RMS vector — so it is directly unit-testable.
SSpeechPresence detectSpeechPresence(const float* samples, size_t n, const SVadConfig& cfg);

class CVad {
  public:
    explicit CVad(const SVadConfig& cfg);

    // Feed mono float samples in [-1,1]. `frameStartMonoMs` is the CLOCK_MONOTONIC
    // time of samples[0]; the stream is assumed contiguous thereafter. Completed
    // segments are appended to `out`. Returns the number of segments appended.
    int push(const float* samples, size_t n, int64_t frameStartMonoMs, std::vector<SSpeechSegment>& out);

    // Force-close any in-progress utterance (e.g. PTT release / EOF) and emit it.
    bool flush(std::vector<SSpeechSegment>& out);

    bool    inSpeech() const { return m_inSpeech; }
    int64_t currentOnsetMs() const { return m_onsetMs; }

    // ---- observability (main-thread reads; snapshot of the last analysed frame) ----
    float lastRms() const { return m_lastRms; }           // RMS of the most recent frame
    float noiseFloor() const { return m_noiseFloor; }     // current adaptive floor estimate
    float threshold() const { return currentThreshold(); }// the live voiced threshold

  private:
    void  processFrame(const float* frame, int64_t frameStartMs, std::vector<SSpeechSegment>& out);
    void  emit(int64_t endMs, std::vector<SSpeechSegment>& out);
    float currentThreshold() const;

    SVadConfig m_cfg;
    size_t     m_frameSamples;
    size_t     m_preRollFrames;

    // Adaptive noise-floor state: a rolling window of recent frame RMS values whose
    // low percentile is the ambient-floor estimate.
    std::deque<float> m_rmsWindow;      // recent per-frame RMS (bounded by m_windowFrames)
    size_t            m_windowFrames = 75;
    float             m_noiseFloor   = 0.f; // last computed floor estimate (observability)
    float             m_lastRms      = 0.f; // RMS of the last frame (observability)

    std::vector<float> m_pending;   // leftover samples not yet a full frame
    int64_t            m_baseMonoMs = 0;
    int64_t            m_sampleIdx  = 0; // total samples consumed into frames
    bool               m_haveBase   = false;

    // Pre-roll ring of recent frames (kept while idle so onset audio isn't clipped).
    std::deque<std::vector<float>> m_preRoll;

    bool               m_inSpeech    = false;
    int                m_voicedRunMs = 0;
    int                m_runGapMs    = 0; // unvoiced ms accumulated INSIDE the onset run
    int                m_silenceRunMs = 0;
    int64_t            m_runStartMs   = 0; // onset candidate time
    int64_t            m_onsetMs      = 0;
    int64_t            m_bufferStartMs = 0; // time of samples[0] in the utterance
    std::vector<float> m_utterance;
};
