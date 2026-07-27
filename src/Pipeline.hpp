#pragma once

#include "Asr.hpp"
#include "Config.hpp"
#include "Transcript.hpp"
#include "Vad.hpp"

// Shared transcript-tier logic used by both the live daemon and the --oneshot path:
// run ASR on a speech segment, and (for wake-word activations) require + strip the
// wake phrase. Kept separate so the same code path is exercised offline in tests.
namespace Pipeline {
    // The SConfig -> SVadConfig mapping, in ONE place. It used to be open-coded by the
    // daemon, the --oneshot path and the loopback test independently, and had already
    // drifted (the offline path silently ran with different gating than the live one).
    SVadConfig vadConfig(const SConfig& cfg);

    // Transcribe one segment. On a wake-word activation, `requireWake` gates the
    // result on the configured wake phrase (stripping it from the text). Returns
    // false if a required wake phrase was absent (utterance rejected).
    bool processSegment(CAsr& asr, const SConfig& cfg, const SSpeechSegment& seg,
                        EActivation activation, bool requireWake, STranscript& out);
}
