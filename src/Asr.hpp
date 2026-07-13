#pragma once

#include "Transcript.hpp"

#include <string>
#include <vector>

// whisper.cpp ASR wrapper. Produces a transcript with WORD-LEVEL absolute
// CLOCK_MONOTONIC timestamps (required — see README spatial-anchoring contract).
// Word timestamps use whisper's DTW token-timestamp alignment when a matching
// alignment-heads preset is known for the model, which is markedly more accurate
// than the t0/t1 heuristic; it falls back to the heuristic otherwise.
class CAsr {
  public:
    CAsr() = default;
    ~CAsr();
    CAsr(const CAsr&)            = delete;
    CAsr& operator=(const CAsr&) = delete;

    struct SParams {
        std::string modelPath;
        std::string language = "en";
        int         threads  = 4;
        bool        translate = false;
    };

    // Loads the model. Returns false (with a clear log) if the model file is
    // missing or fails to load. `err` receives a user-facing message.
    bool load(const SParams& params, std::string& err);
    bool loaded() const { return m_ctx != nullptr; }

    // True if DTW word-timestamp alignment is active for the loaded model.
    bool dtwActive() const { return m_dtw; }

    // Transcribe 16 kHz mono float samples. `bufferStartMs` is the CLOCK_MONOTONIC
    // time of samples[0]; all word timestamps are made absolute by adding it.
    // `onsetMs`/`endMs` are copied into the transcript verbatim.
    STranscript transcribe(const std::vector<float>& samples, int64_t bufferStartMs, int64_t onsetMs, int64_t endMs, EActivation activation);

  private:
    void*  m_ctx = nullptr; // struct whisper_context*
    SParams m_params;
    bool   m_dtw = false;
};
