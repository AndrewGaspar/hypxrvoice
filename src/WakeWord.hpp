#pragma once

#include <string>
#include <vector>

// Wake-word tier. In V2 the default backend is "vad-transcribe": while ARMED, the
// VAD segments speech and the ASR transcribes it, then this pure matcher decides
// whether the utterance began with the wake phrase and strips it, leaving the
// command text. It is fully local and dependency-free.
//
// SEAM (WP-V7): a cheaper, pre-ASR "openwakeword" ONNX backend (config
// wake.backend="openwakeword") would gate BEFORE transcription to save CPU while
// armed. That backend needs an ONNX runtime (deliberately not vendored here — see
// report). The daemon selects a backend by name; only vad-transcribe is built in V2.
namespace WakeWord {
    // Lowercase, strip punctuation, collapse whitespace into tokens.
    std::vector<std::string> normalize(const std::string& text);

    // Levenshtein edit distance between two strings.
    int editDistance(const std::string& a, const std::string& b);

    // Does `transcript` begin with `phrase` (within `fuzz` edits per phrase word)?
    // On success, `stripped` receives the remaining command text (phrase removed).
    // `fuzz` is the total edit-distance budget scaled by the phrase word count.
    bool matchPrefix(const std::string& transcript, const std::string& phrase, int fuzz, std::string& stripped);
}
