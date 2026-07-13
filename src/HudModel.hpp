#pragma once

#include "Command.hpp"
#include "Config.hpp"
#include "Executor.hpp"

#include <string>
#include <vector>

// WP-V5 feedback tier — the PURE view model for the in-headset HUD. Nothing here
// touches OpenXR, EGL, GL, or a font: it turns the intent/executor records into a
// small structured "what to show" description (title, coloured text lines, a
// confidence bar, an approximated/dry-run marker) plus fade timing. The renderer
// (HudText) rasterises it and the overlay subprocess submits it. Keeping this layer
// pure is what makes the HUD reviewable offline (--hud-preview) and unit-testable.

enum class EHudState {
    Hidden,    // nothing to show.
    Listening, // mic is open; showing "listening" and any partial transcript.
    Action,    // a recognised command — the veto window before/while it executes.
    Clarify,   // ambiguous — a question + candidate targets.
    Error,     // recognised but could not be carried out (refused/unsupported).
};
const char* hudStateName(EHudState s);

// A semantic colour role; HudText maps each to concrete RGBA. Kept abstract so the
// palette lives in one place and the model stays render-agnostic.
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

    // Fade envelope (renderer-clock ms). holdMs < 0 => stay until replaced/hidden
    // (used for Listening). Rise+hold+fade for transient panels (Action/Clarify/Error).
    int   riseMs      = 110;
    int   holdMs      = 2600;
    int   fadeMs      = 450;
    float opacityCeil = 0.92f;

    bool empty() const { return state == EHudState::Hidden; }
};

// Layer opacity for a frame shown `elapsedMs` ago, following the rise/hold/fade
// envelope. Pure — the overlay subprocess calls this every frame for a free,
// texture-upload-free fade (color-scale-bias .a). Returns [0, opacityCeil].
float hudOpacity(const SHudView& v, int64_t elapsedMs);

// --- builders: the intent/executor records -> an SHudView. All pure. ---

// The listening panel (mic just opened). `partial` is any transcript-so-far to show
// (empty at first). holdMs<0: it persists until an action/clarify replaces it or
// onListeningStop hides it.
SHudView hudForListening(const std::string& partial, const SConfig& cfg);

// The command panel — the user's veto window. Maps verb/target/targetSource,
// confidence, approximated/dry-run, and Clarify candidates into a panel. This is the
// single richest builder and the one most worth testing.
SHudView hudForAction(const SAction& a, const SExecPlan& plan, const SConfig& cfg);

// A short human phrase for a verb+target (shared by HUD title and TTS). e.g.
// "moving XR-code", "which firefox?", "opening browser".
std::string hudActionPhrase(const SAction& a);
