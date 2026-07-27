#pragma once

#include "Command.hpp"
#include "Config.hpp"
#include "Executor.hpp"
#include "Transcript.hpp"

#include <functional>

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
    // `utterance` is the transcript the action came from. It matters for the EVerb::None
    // case: instead of hiding the HUD (the WP-V6 silent-rejection bug) the sink shows the
    // transcript back with a "no command" note. Defaulted so the offline/oneshot callers
    // and tests are unaffected.
    void emitAction(const SAction& a, const SExecPlan& plan, const SConfig& cfg,
                    const std::string& utterance = "");

    // A capture window that produced no command at all — the pre-intent rejections:
    // nothing heard (VAD found no speech / ASR empty, `transcript` == ""), or a
    // transcript that never reached the intent tier. `note` overrides the panel's
    // default second line for a more specific cause. Never silent: an explicitly
    // requested (PTT) window must always tell the user what happened.
    void emitRejected(const std::string& transcript, const SConfig& cfg,
                      const std::string& note = "");

    // ---- WP-V5 runtime (daemon-only; oneshot/tests never call these, so they never
    // spawn an XR client). start once at daemon init, stop at teardown, poll each tick.
    void startRuntime(const SConfig& cfg);
    void stopRuntime();
    void pollRuntime();

    // HUD listening panel: show on mic open, hide on close without a command.
    void onListeningStart(const SConfig& cfg);
    void onListeningStop(const SConfig& cfg);

    // TEST-ONLY seam: redirect the notify-send fallback to a sink so the degraded path
    // (HUD daemon unreachable) is assertable without spawning notify-send. Passing nullptr
    // restores the real notify-send behaviour.
    void _setNotifySinkForTest(std::function<void(const std::string&, const std::string&)> sink);
}
