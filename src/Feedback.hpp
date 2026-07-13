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

    // The V4 command sink, now V5-wired: emits the machine-readable line + log, drives
    // the in-headset HUD (via the overlay subprocess) and terse TTS, and keeps
    // notify-send as the degraded fallback when the HUD is unavailable.
    void emitAction(const SAction& a, const SExecPlan& plan, const SConfig& cfg);

    // ---- WP-V5 runtime (daemon-only; oneshot/tests never call these, so they never
    // spawn an XR client). start once at daemon init, stop at teardown, poll each tick.
    void startRuntime(const SConfig& cfg);
    void stopRuntime();
    void pollRuntime();

    // HUD listening panel: show on mic open, hide on close without a command.
    void onListeningStart(const SConfig& cfg);
    void onListeningStop(const SConfig& cfg);
}
