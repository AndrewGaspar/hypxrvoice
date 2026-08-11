#pragma once

#include <string>
#include <vector>

// WP-V4 intent tier: the strict, closed command schema the intent model emits and
// the executor consumes. Nothing here shells out or touches the compositor — it is
// a pure data description plus (de)serialization, so it is fully unit-testable.
//
// Design (see docs/openxr/research/VOICE-CONTROL.md §1.5 and the locked interaction
// model): a small, closed set of verbs, each mapping to an allowlisted `hyprctl`
// invocation in the executor. Monitor targets are ENUMERATED from the live desktop
// snapshot; the model never invents a name — an unknown target degrades to Clarify.

// The closed verb set. Extending it is a deliberate, reviewed act (the executor's
// allowlist must grow in lockstep).
enum class EVerb {
    None,       // out of scope — explicitly NOT a compositor command. Emits nothing.
    Clarify,    // ambiguous/underspecified — ask, do not guess-actuate.
    Pick,       // "pick this monitor up" — start carrying it (follows head).
    Place,      // "place it here" / "drop it" — release/freeze at current pose.
    MoveDist,   // "closer" / "further" — push/pull along the view ray (deltaM).
    Center,     // "center it in front of me".
    Dock,       // "dock it" / "dock here".
    Undock,     // "undock" / "pick it up and follow".
    Follow,     // "have X follow me" — adaptive/roam decorator.
    Anchor,     // "anchor it to my head/body" / "world-lock it".
    HandInput,  // "hands on/off" — conditional hand-input toggle.
    MonitorView, // "hide/show/toggle monitor view" — global XR quad presentation gate.
    LaunchApp,  // "open the browser" — allowlisted desktop-entry launch.

    // ---- plain window management (round-2: the phrasings the user actually spoke) ----
    // These leave the XR layer alone and drive Hyprland itself. All three are
    // non-destructive and reversible — nothing here can close, kill, or relocate a
    // window — which is why they are permitted by default (executor.allow_window).
    Focus,      // "focus the browser" — raise/focus a LIVE window.
    Fullscreen, // "make this window fullscreen" — toggle fullscreen on a window.
    Workspace,  // "workspace three" — switch to a numbered workspace.
    // "move terminal to the left monitor" / "send the browser to workspace 3" — relocate
    // ONE live window to another output or workspace. Reversible like the rest of this
    // group (the window is not closed, resized, or hidden), so it shares allow_window.
    MoveWindow,
    // "move workspace 4 to this monitor" — relocate a WHOLE numbered workspace onto
    // another output (`dispatch moveworkspacetomonitor <N> <mon>`). Distinct from
    // MoveWindow: the subject is a workspace index, never a window, which is exactly
    // the confusion that made round 5's misfire dangerous.
    MoveWorkspace,
    // "create a monitor here" — materialize a NEW runtime XR monitor
    // (`openxr create <name> <WxH>@<Hz>`), optionally placed at the projected gaze
    // point. Runtime-created monitors are never touched by config reconciliation.
    CreateMonitor,
};

const char* verbName(EVerb v);
EVerb       verbFromName(const std::string& s); // unknown -> EVerb::None.

// Anchor / follow reference frame. Mirrors the compositor's anchor grammar.
enum class EAnchorMode { Unset, Local, Head, Body, DeviceLeft, DeviceRight };
const char* anchorModeName(EAnchorMode m); // "local"|"head"|"body"|"device:left"|...
EAnchorMode parseAnchorMode(const std::string& s);

// How the action's target monitor was resolved — surfaced to the feedback tier so
// the HUD can say "closing XR-chat (you were looking at it)" vs "(best guess)".
enum class ETargetSource {
    None,      // no monitor target (e.g. HandInput, LaunchApp).
    Active,    // "active" — defer resolution to the compositor's selection order.
    Named,     // an explicit monitor name the user said and we matched verbatim.
    Semantic,  // resolved from the desktop snapshot ("the coding monitor" -> XR-code).
    Deixis,    // resolved from gaze-at-word-time ("this"/"here").
};
const char* targetSourceName(ETargetSource s);

// The resolved gaze sample that backed a deictic reference, kept for feedback +
// the (proposed) place-at-pose executor path. All fields in LOCAL_FLOOR meters.
struct SGazeResolution {
    bool    valid       = false;
    int     monitorId   = -1;     // -1 = looking at passthrough (missed every quad).
    std::string name;             // gaze candidate monitor name, if any.
    // The HEAD ORIGIN at word time — where the user's EYES were, NOT where they were
    // looking. Round 5's first live-fire proved why the distinction matters: the
    // executor passed `pos` straight to `openxr place`, which dropped the monitor
    // inside the user's head. Nothing that positions geometry may read `pos`.
    double  pos[3]      = {0, 0, 0};
    double  quat[4]     = {0, 0, 0, 1};
    double  forward[3]  = {0, 0, 0};
    // The PROJECTED point the deixis designates — this is what "here" means and the
    // ONLY field a placement command may use. It is the gaze-ray/monitor intersection
    // when the compositor reports one, else head + normalize(forward)*place_distance_m,
    // and it is always at least place_min_distance_m away from the head.
    double  place[3]     = {0, 0, 0};
    bool    placeFromHit = false; // true = a real ray/quad intersection, not a projection.
    double  placeDistM   = 0.0;   // |place - pos|, the distance actually used.
    double  dwellSec    = 0.0;
    int64_t matchedMs   = 0;      // the ring sample actually used.
    int64_t requestedMs = 0;      // the word timestamp we asked for.
    int64_t ageMs       = 0;      // requested - matched (staleness of the match).
    bool    stable      = false;  // the stability window agreed on this candidate.
    int     agreeCount  = 0;      // how many window samples agreed (for confidence).
    int     sampleCount = 0;      // how many window samples were taken.
    std::string toJson() const;
};

// One fully-resolved command. This is the executor's input and the feedback tier's
// display record. It is produced by an intent backend (rule-based or llama) after
// deixis + semantic resolution against the live snapshot.
struct SAction {
    EVerb         verb   = EVerb::None;
    std::string   target;                        // monitor name, or "active", or "".
    ETargetSource targetSource = ETargetSource::None;

    EAnchorMode   anchor = EAnchorMode::Unset;   // Anchor/Follow.
    std::string   sub;                           // sub-action: on|off|toggle|auto|here|head|body.
    double        deltaM = 0.0;                  // MoveDist push(+)/pull(-) meters.
    std::string   app;                           // LaunchApp: allowlist key (resolved to a command by the executor).

    // Focus/Fullscreen: the address of a LIVE window taken verbatim from the compositor
    // snapshot ("0x55f0…"). An address, not a class, because `focuswindow class:X` treats
    // X as a REGEX — an address is exact and has a trivially checkable shape. Empty means
    // "whatever is focused", which is the correct reading of "make it fullscreen".
    std::string   windowAddress;
    std::string   windowLabel;                   // human name for the HUD ("firefox").
    // Workspace: the index WE parsed from the utterance, 1..99. 0 = none. For MoveWindow
    // a non-zero value means the destination is a WORKSPACE rather than a monitor.
    int           workspace = 0;

    SGazeResolution gaze;                        // populated when targetSource==Deixis.

    // 0..1 self-reported confidence. <1 marks a best-guess the feedback tier should
    // surface. Clarify/None carry their own semantics regardless of this number.
    double        confidence = 1.0;

    // Clarify only: the question and the candidate targets to disambiguate.
    std::string              clarifyQuestion;
    std::vector<std::string> clarifyCandidates;

    // Free-form provenance note (which rule/model path produced this) for logs/HUD.
    std::string   note;
    // The transcript text this action came from (for feedback + logs).
    std::string   utterance;

    bool actionable() const { return verb != EVerb::None && verb != EVerb::Clarify; }

    std::string toJson() const;
};

// Parse a single strict-JSON action object (the intent model's output, or a fixture)
// into an SAction. Unknown verbs -> EVerb::None. Returns false only on malformed JSON
// (a well-formed object with an unknown verb still parses, as None). `err` is set on
// failure. This is the boundary that keeps model output from ever becoming a raw
// command line — everything downstream reads the typed SAction, never the JSON.
bool parseAction(const std::string& json, SAction& out, std::string& err);
