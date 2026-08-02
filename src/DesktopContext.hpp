#pragma once

#include <functional>
#include <string>
#include <vector>

// WP-V4: the desktop-context snapshot taken at utterance time. It fuses three live,
// READ-ONLY compositor queries — `hyprctl monitors -j`, `hyprctl clients -j`, and
// `hyprctl -j openxr status` — into a compact model of what monitors exist, which
// apps live on them, and their XR anchor state. Two consumers:
//   1. digest()        -> a few hundred tokens injected into the intent prompt so the
//                          model resolves "the coding monitor" from ENUMERATED names.
//   2. resolveMonitor()-> the rule-based backend's semantic resolver, and the
//                          post-parse validator that rejects an invented target.
// Parsing is pure over JSON strings (testable with fixtures, no live compositor).

// Where a spoken "the LEFT monitor" / "the RIGHT monitor" points. Resolved against the
// live layout geometry, never against a name — the user is describing what they can see.
enum class ESpatialRef { None, Left, Right };

struct SMonitorInfo {
    std::string name;          // e.g. "XR-code" — the enumerated target token.
    int         id       = -1;
    bool        focused  = false;
    // Layout position, straight from `hyprctl monitors -j`. This is what makes "the left
    // monitor" resolvable: leftmost/rightmost by x, with no guessing from names.
    int         x        = 0;
    int         y        = 0;
    int         width    = 0;
    int         height   = 0;
    bool        xr       = false;   // an XR monitor (name XR-* or present in openxr status).
    std::string anchorMode;         // "local"|"head"|"body"|... (XR only).
    bool        hovered  = false;   // ray-hovered right now (deixis hint).
    bool        grabbed  = false;   // controller/hand-grabbed right now.
    std::string workspace;          // active workspace name on this monitor.
    std::vector<std::string> appClasses; // window classes present on this monitor.
    std::vector<std::string> appTitles;  // window titles present on this monitor.
};

// One live toplevel window. Carried alongside the per-monitor app lists because the
// window verbs (focus / fullscreen) need to name ONE window, not a monitor, and they
// need an exact handle for it.
struct SWindowInfo {
    // The compositor's own handle, "0x55f0abcd1234". Exact and unambiguous — unlike
    // `class:X`, which Hyprland interprets as a REGEX, so a class carrying a metachar
    // could select the wrong window (or none). Never derived from transcript text.
    std::string address;
    std::string cls;
    std::string title;
    int         monitorId      = -1;
    // Hyprland's recency rank: 0 is the focused window, 1 the one before it, and so on.
    // Used to break a tie deterministically toward "the one you were just using".
    int         focusHistoryId = 1 << 30;
    bool        focused        = false;
};

// Result of a semantic window resolution ("the browser" -> a live firefox window).
struct SWindowMatch {
    bool                     matched    = false;
    std::string              address;           // exact handle for the executor.
    std::string              label;             // human name for the HUD ("firefox").
    double                   confidence = 0.0;  // 0..1.
    std::vector<std::string> candidates;        // distinct labels when genuinely ambiguous.
};

// Result of a semantic monitor resolution.
struct SMonitorMatch {
    bool                     matched    = false;
    std::string              name;              // best monitor name.
    double                   confidence = 0.0;  // 0..1.
    std::vector<std::string> candidates;        // when ambiguous, the tied names.
};

struct SDesktopContext {
    std::vector<SMonitorInfo> monitors;
    std::vector<SWindowInfo>  windows;
    bool        xrAvailable = false;
    std::string xrState;                 // openxr status.state, if available.

    // Pure parse from the three JSON blobs. Any blob may be empty/invalid; the
    // snapshot degrades (an absent openxr blob just means no XR annotations).
    static SDesktopContext parse(const std::string& monitorsJson,
                                 const std::string& clientsJson,
                                 const std::string& openxrJson);

    // Enumerated, valid monitor names — the ONLY targets a command may reference.
    std::vector<std::string> monitorNames() const;
    bool                     hasMonitor(const std::string& name) const;
    const SMonitorInfo*      find(const std::string& name) const;

    // Is this window handle live right now? The executor's last check before it
    // dispatches at an address, so a stale handle is refused instead of actuated.
    bool                     hasWindow(const std::string& address) const;

    // Resolve "the left/right monitor" against the LAYOUT: leftmost / rightmost by x.
    // Unmatched when there are fewer than two monitors (nothing is "the left one" when
    // there is only one). When two monitors share the extreme x the answer is genuinely
    // ambiguous, so `candidates` is filled and confidence drops — the caller turns that
    // into a Clarify rather than picking.
    SMonitorMatch resolveSpatialMonitor(ESpatialRef ref) const;

    // The single ray-hovered monitor, if exactly one (a cheap deixis fallback when
    // gaze history is unavailable). Null otherwise.
    const SMonitorInfo*      hoveredMonitor() const;

    // Semantic resolution: match a spoken phrase ("the coding monitor", "youtube")
    // against monitor names + the apps living on them. Returns the best monitor, a
    // confidence, and — when two-plus tie — the candidate set for a Clarify.
    SMonitorMatch resolveMonitor(const std::string& phrase) const;

    // Semantic resolution of a spoken window reference against the LIVE window list:
    // an app class said outright ("firefox"), a generic noun from a small closed table
    // ("the browser", "the editor", "the terminal"), or a title keyword. Only ever
    // returns a window that exists right now, so the executor can never be handed a
    // handle the compositor does not know. Ties resolve toward the most recently focused
    // window; `candidates` is filled only when the tie spans DIFFERENT apps, which is
    // the only case where asking the user is actually informative.
    SWindowMatch resolveWindow(const std::string& phrase) const;

    // The focused window, if the snapshot identified one. Null otherwise.
    const SWindowInfo* focusedWindow() const;

    // A free name for a NEW runtime XR monitor: "XR-<n>", the lowest n from 2 up that
    // collides with nothing live (XR-1 is conventionally the primary). Generated by US
    // from a closed shape, so no transcript text can ever reach a create argv. Empty
    // when the space is exhausted.
    std::string nextXrMonitorName() const;
    // A pixel mode for a new XR monitor ("WxH@60"), matching the live XR monitors when
    // they report a sane one, else 1920x1080.
    std::string newXrMonitorMode() const;

    // Compact, deterministic digest for the intent prompt (bounded by caps).
    std::string digest(int maxMonitors = 16, int maxAppsPerMon = 6) const;
};

// Live snapshot via hyprctl. `runQuery` runs an argv and returns stdout (injectable
// so tests never touch a real compositor). The default runner uses popen(hyprctl).
using QueryFn = std::function<std::string(const std::vector<std::string>& argv)>;
SDesktopContext snapshotDesktop(const QueryFn& runQuery);
std::string     defaultHyprctlQuery(const std::vector<std::string>& argv);
