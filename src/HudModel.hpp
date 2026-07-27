#pragma once

#include "Command.hpp"
#include "Config.hpp"
#include "Executor.hpp"

#include <string>
#include <vector>

// WP-V5/WP-H8 feedback tier — the PURE view model for the in-headset HUD. Nothing
// here touches OpenXR, EGL, GL, D-Bus, or a font: it turns the intent/executor records
// into a small structured "what to show" description (title, coloured text lines, a
// confidence bar, an approximated/dry-run marker) plus the fade-envelope timing.
//
// WP-H8: rendering + fade compositing now live in the shared `hypxrhud` daemon.
// hypxrvoice is a pure D-Bus client (see HudClient): it maps this SHudView onto the
// daemon's `a{sv}` panel props (HudClient::hudPropsFromView) and pushes it. The
// envelope fields below (rise/hold/fade) are forwarded verbatim as panel props so the
// daemon applies the same rise→hold→fade; hypxrvoice no longer computes opacity itself.
// Keeping this layer pure is what makes the mapping unit-testable with no bus.

enum class EHudState {
    Hidden,    // nothing to show.
    Listening, // mic is open; showing "listening" and any partial transcript.
    Action,    // a recognised command — the veto window before/while it executes.
    Clarify,   // ambiguous — a question + candidate targets.
    Error,     // recognised but could not be carried out (refused/unsupported).
    Rejected,  // the window produced no command (nothing heard, or nothing parsed).
};
const char* hudStateName(EHudState s);

// A semantic colour role, forwarded to hypxrhud as a small colorRole int (see
// HudClient::hudColorRole); the daemon maps each to concrete RGBA. Kept abstract so the
// palette lives in one place (hypxrhud) and this model stays render-agnostic.
enum class EHudColor {
    Normal, // primary foreground.
    Dim,    // secondary / hint text.
    Accent, // the verb / focus.
    Good,   // success / high confidence.
    Warn,   // caution / low confidence / approximated.
    Bad,    // error.
};

struct SHudLine {
    std::string text;
    EHudColor   color = EHudColor::Normal;
    bool        big   = false; // render larger (the title line).
};

// A fully-resolved HUD frame. Timing is expressed relative to the moment the frame
// is shown (the renderer stamps its own monotonic clock on receipt), so the model
// never needs the compositor clock.
struct SHudView {
    EHudState             state = EHudState::Hidden;
    std::vector<SHudLine> lines;

    float confidence  = -1.f;  // [0,1] draws a bar; <0 = no bar.
    bool  approximated = false; // executor used a fallback for a missing capability.
    bool  dryRun       = false; // executor will actuate nothing (a badge, not an error).

    // Fade envelope (hypxrhud panel props, ms). holdMs < 0 => stay until replaced/
    // hidden (used for Listening; the daemon's envelope treats hold<0 as persistent).
    // Rise+hold+fade for transient panels (Action/Clarify/Error). Forwarded as
    // rise_ms/hold_ms/fade_ms props; the daemon owns the actual opacity compositing.
    int   riseMs      = 110;
    int   holdMs      = 2600;
    int   fadeMs      = 450;

    bool empty() const { return state == EHudState::Hidden; }
};

// --- builders: the intent/executor records -> an SHudView. All pure. ---

// The listening panel (mic just opened). `partial` is any transcript-so-far to show
// (empty at first). holdMs<0: it persists until an action/clarify replaces it or
// onListeningStop hides it.
SHudView hudForListening(const std::string& partial, const SConfig& cfg);

// The command panel — the user's veto window. Maps verb/target/targetSource,
// confidence, approximated/dry-run, and Clarify candidates into a panel. This is the
// single richest builder and the one most worth testing.
SHudView hudForAction(const SAction& a, const SExecPlan& plan, const SConfig& cfg);

// The rejection panel — a capture window that produced NO command.
//
// WHY (WP-V6 live-validation finding): hudForAction() used to answer EVerb::None with a
// Hidden view, so a transcript the parser could not use left the user staring at
// "listening…" with no way to tell whether the mic, the ASR, or the grammar was at
// fault. Six consecutive utterances gave zero feedback. A rejected window now SAYS so.
//
// `transcript` is what was heard, verbatim ("" when nothing was heard at all — VAD found
// no speech or ASR returned empty). `note` overrides the default second line, for
// rejections with a more specific cause (e.g. no ASR model loaded).
SHudView hudForRejection(const std::string& transcript, const std::string& note, const SConfig& cfg);

// A short human phrase for a verb+target (shared by HUD title and TTS). e.g.
// "moving XR-code", "which firefox?", "opening browser".
std::string hudActionPhrase(const SAction& a);
