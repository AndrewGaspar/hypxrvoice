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
    float energyThreshold = 0.012f; // RMS floor: a frame below this is ALWAYS silence
    int   startMs         = 150;    // sustained voiced to declare onset
    int   endMs           = 600;    // sustained silence (hangover) to declare end
    int   maxUtteranceMs  = 12000;
    int   preRollMs       = 300;    // audio retained before the onset instant
    int   frameMs         = 20;     // analysis window

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
};

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
    int                m_silenceRunMs = 0;
    int64_t            m_runStartMs   = 0; // onset candidate time
    int64_t            m_onsetMs      = 0;
    int64_t            m_bufferStartMs = 0; // time of samples[0] in the utterance
    std::vector<float> m_utterance;
};
