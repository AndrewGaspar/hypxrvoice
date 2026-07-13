#pragma once

#include "Command.hpp"
#include "Config.hpp"
#include "Executor.hpp"
#include "Transcript.hpp"

// V2 feedback tier: transcripts go to stdout (JSON line) + the log, and optionally a
// notify-send toast. The richer feedback tier (in-headset HUD + piper TTS) is later
// work (WP-V5); the seam is this single sink.
//
// WP-V5 CONTRACT: the HUD/TTS tier replaces the bodies below (or subscribes to the
// same call sites). emitAction() is the single point every recognized command flows
// through — it carries the human-readable phrasing (action.note / verb), the resolved
// target + how it was resolved (targetSource, gaze staleness), a confidence in [0,1]
// to render as certainty, the clarify question/candidates when disambiguation is
// needed, and the executor PLAN (dry-run or live, approximated or exact) so the HUD
// can show precisely what will run. No new state is required of the caller.
namespace Feedback {
    void emitTranscript(const STranscript& t, const SConfig& cfg);

    // The V4 command sink. `plan` reflects what the executor will do (its steps are
    // already validated). Emits a JSON line + log + optional toast; V5 swaps the body.
    void emitAction(const SAction& a, const SExecPlan& plan, const SConfig& cfg);
}
