#pragma once

#include "Transcript.hpp"

#include <string>
#include <vector>

// whisper.cpp ASR wrapper. Produces a transcript with WORD-LEVEL absolute
// CLOCK_MONOTONIC timestamps (required — see README spatial-anchoring contract).
// Word timestamps use whisper's DTW token-timestamp alignment when a matching
// alignment-heads preset is known for the model, which is markedly more accurate
// than the t0/t1 heuristic; it falls back to the heuristic otherwise.
//
// WP-V7: it also carries a VOCABULARY BIAS — a short script of plausible commands over
// the windows and monitors that are live right now, handed to whisper as prompt_tokens
// so the decoder stops guessing "clicks" at Plex. See VocabBias.hpp for the prompt
// format and the A/B behind it; the bias string is built by the daemon at capture-window
// open (never mid-transcribe) and installed here with setVocabBias().
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

    // Install the live vocabulary bias. Tokenized ONCE here (whisper_tokenize) rather
    // than per transcription, so (a) the per-utterance cost is a memcpy and (b) the
    // budget is exact instead of a chars-per-token guess. `maxTokens` is clamped to
    // whisper's own ceiling of n_text_ctx/2; when the prompt is longer, its TAIL is
    // kept — that is the end of the exemplar script, which carries the command-verb
    // vocabulary, and it is also the half whisper itself would keep. Passing "" clears
    // the bias. Safe to call on every capture-window open; a no-op when the text is
    // unchanged. MAIN THREAD ONLY (same thread as transcribe()).
    // Returns the prompt's FULL token count before any truncation — the caller needs
    // that, not the installed count, to tell "it fit" from "it was cut" and shrink the
    // vocabulary instead (see CDaemon::refreshVocabBias). 0 means nothing was installed.
    int  setVocabBias(const std::string& text, int maxTokens = 96);
    // The prompt currently installed (for `status`, logs and the echo guard).
    const std::string& vocabBias() const { return m_biasText; }
    // How many whisper tokens it actually cost.
    int                vocabBiasTokens() const { return static_cast<int>(m_biasTokens.size()); }

    // Transcribe 16 kHz mono float samples. `bufferStartMs` is the CLOCK_MONOTONIC
    // time of samples[0]; all word timestamps are made absolute by adding it.
    // `onsetMs`/`endMs` are copied into the transcript verbatim.
    //
    // `useVocabBias` is the caller's verdict on whether this buffer is confidently
    // speech. It matters: an initial prompt makes whisper CONFIDENT, and on a buffer
    // with no speech in it that confidence is spent inventing text — measured on real
    // Quest-mic ambient, biased decoding turned "[BLANK_AUDIO]" (no_speech_prob 0.89)
    // into "Subscribe to our channel for more videos!" (no_speech_prob 0.000) and took
    // 4x as long doing it. The prompt also collapses no_speech_prob generally, so
    // whisper's own no-speech verdict cannot be the guard. Pipeline::processSegment
    // therefore measures the buffer first and passes false for a near-silent one.
    STranscript transcribe(const std::vector<float>& samples, int64_t bufferStartMs, int64_t onsetMs, int64_t endMs, EActivation activation,
                           bool useVocabBias = true);

  private:
    void*  m_ctx = nullptr; // struct whisper_context*
    SParams m_params;
    bool   m_dtw = false;

    std::string          m_biasText;   // verbatim prompt (also the echo-guard reference)
    std::vector<int32_t> m_biasTokens; // whisper_token, truncated to the budget
    int                  m_biasBudget = 96;  // token budget the text was tokenized under
    int                  m_biasRawTokens = 0; // token count BEFORE truncation
};
