#pragma once

#include "Command.hpp"
#include "Config.hpp"
#include "Executor.hpp"

#include <string>

// WP-V5 terse text-to-speech. Speech confirms only what the user cannot see on the
// HUD: errors, clarify questions, and (in "all" mode) every action. NO ONNX/piper
// (the WP-V2 constraint); we shell out to the `espeak-ng` binary if it is on PATH,
// else TTS is cleanly disabled. The phrase selection is a pure function so it is
// fully unit-testable; only speak() has a side effect.

namespace Tts {
    // Choose the utterance for an action under the configured mode, or "" for
    // silence. Pure. Modes: "off" (always ""), "errors" (refusals + clarify only),
    // "all" (also success confirmations). Phrasings are terse: "moving XR-code",
    // "which firefox?", "can't, no such monitor".
    std::string phraseFor(const SAction& a, const SExecPlan& plan, const SConfig& cfg);

    // True if the espeak-ng binary is resolvable on PATH (probed once, cached).
    bool available();

    // Speak `text` via espeak-ng using cfg voice/rate. No-op if text is empty, the
    // mode is off, or espeak-ng is unavailable. Non-blocking (fire-and-forget child,
    // reaped without waiting on synthesis). Safe to call from the daemon loop.
    void speak(const std::string& text, const SConfig& cfg);
}
