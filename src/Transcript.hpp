#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The output structure of the transcript tier — the deliverable of WP-V2. Later
// work packages (V4 intent parsing, V5 in-headset HUD, V6 cloud escalation) consume
// this; nothing here interprets the command.
//
// The spatial-anchoring contract lives in `onsetMs`: a CLOCK_MONOTONIC millisecond
// timestamp captured at VAD speech-onset (or PTT-press for held speech), which the
// compositor resolves to a head pose via `hyprctl openxr gaze at <onsetMs>`.
struct SWord {
    std::string text;
    // Per-word timestamps in CLOCK_MONOTONIC ms, when the ASR provides them. These
    // are absolute (already offset by the segment's capture-start monotonic time),
    // so they are directly comparable to onsetMs and to the compositor clock.
    int64_t     startMs = 0;
    int64_t     endMs   = 0;
    // Model confidence in [0,1] if available, else -1.
    float       prob    = -1.f;
};

enum class EActivation {
    PushToTalk, // opened by an explicit PTT command
    WakeWord,   // opened by the wake-word tier
    Oneshot,    // fed from a file via --oneshot (offline / test path)
};

inline const char* activationName(EActivation a) {
    switch (a) {
        case EActivation::PushToTalk: return "ptt";
        case EActivation::WakeWord:   return "wake";
        case EActivation::Oneshot:    return "oneshot";
    }
    return "?";
}

struct STranscript {
    // The recognized text (wake phrase already stripped in wake-word activations).
    std::string        text;
    std::vector<SWord> words;

    // CLOCK_MONOTONIC ms of speech onset. THE anchoring timestamp. Always set.
    int64_t onsetMs = 0;
    // CLOCK_MONOTONIC ms of speech end (VAD offset / PTT release / EOF).
    int64_t endMs = 0;
    // Wall-clock epoch ms when this transcript was produced (for logs/JSON only,
    // NOT for anchoring — the compositor clock is monotonic, not wall-clock).
    int64_t wallMs = 0;

    EActivation activation = EActivation::PushToTalk;

    // Wake-word activations only: the phrase that triggered, before stripping.
    std::string wakePhrase;

    // Serialize to a single-line JSON object (stdout / log / IPC). Hand-rolled so
    // the transcript tier carries no JSON-library dependency of its own.
    std::string toJson() const;

    bool empty() const { return text.empty(); }
};
