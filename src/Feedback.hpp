#pragma once

#include "Config.hpp"
#include "Transcript.hpp"

// V2 feedback tier: transcripts go to stdout (JSON line) + the log, and optionally a
// notify-send toast. The richer feedback tier (in-headset HUD + piper TTS) is later
// work (WP-V5); the seam is this single sink.
namespace Feedback {
    void emitTranscript(const STranscript& t, const SConfig& cfg);
}
