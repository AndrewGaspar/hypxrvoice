#pragma once

#include "Asr.hpp"
#include "Config.hpp"
#include "Transcript.hpp"
#include "Vad.hpp"

#include <string>

// Shared transcript-tier logic used by both the live daemon and the --oneshot path:
// run ASR on a speech segment, and (for wake-word activations) require + strip the
// wake phrase. Kept separate so the same code path is exercised offline in tests.
namespace Pipeline {
    // The SConfig -> SVadConfig mapping, in ONE place. It used to be open-coded by the
    // daemon, the --oneshot path and the loopback test independently, and had already
    // drifted (the offline path silently ran with different gating than the live one).
    SVadConfig vadConfig(const SConfig& cfg);

    // WP-V7 hallucination guard. An initial prompt makes whisper CONFIDENT, and on a
    // buffer with no speech in it that confidence is spent inventing text: measured on
    // real Quest-mic ambient, the bias turned "[BLANK_AUDIO]" (no_speech_prob 0.89) into
    // a fluent "Subscribe to our channel for more videos!" (no_speech_prob 0.000), at 4x
    // the decode time. Note what that means: the prompt COLLAPSES no_speech_prob, so
    // whisper's own no-speech verdict cannot be the guard against its own prompt.
    //
    // The guard is therefore applied BEFORE the decode, on the audio: reuse the VAD's
    // own presence measurement (detectSpeechPresence — the same pure function that
    // decides whether a PTT window is worth transcribing at all) and bias only a buffer
    // that is confidently speech. A near-silent buffer is transcribed unbiased, exactly
    // as it is today, and the existing no-speech path rejects it. Costs one RMS pass.
    bool vocabBiasAllowed(const SConfig& cfg, const SSpeechSegment& seg);

    // Is `text` nothing but an echo of the bias prompt? The other classic prompted-ASR
    // failure: the decoder replays its own context instead of the audio. Cheap, exact,
    // and it can only ever fire on a transcript we ourselves put in the prompt.
    bool isPromptEcho(const std::string& text, const std::string& prompt);

    // Transcribe one segment. On a wake-word activation, `requireWake` gates the
    // result on the configured wake phrase (stripping it from the text). Returns
    // false if a required wake phrase was absent (utterance rejected).
    bool processSegment(CAsr& asr, const SConfig& cfg, const SSpeechSegment& seg,
                        EActivation activation, bool requireWake, STranscript& out);
}
