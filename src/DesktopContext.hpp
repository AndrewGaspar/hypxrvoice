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

struct SMonitorInfo {
    std::string name;          // e.g. "XR-code" — the enumerated target token.
    int         id       = -1;
    bool        focused  = false;
    bool        xr       = false;   // an XR monitor (name XR-* or present in openxr status).
    std::string anchorMode;         // "local"|"head"|"body"|... (XR only).
    bool        hovered  = false;   // ray-hovered right now (deixis hint).
    bool        grabbed  = false;   // controller/hand-grabbed right now.
    std::string workspace;          // active workspace name on this monitor.
    std::vector<std::string> appClasses; // window classes present on this monitor.
    std::vector<std::string> appTitles;  // window titles present on this monitor.
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

    // The single ray-hovered monitor, if exactly one (a cheap deixis fallback when
    // gaze history is unavailable). Null otherwise.
    const SMonitorInfo*      hoveredMonitor() const;

    // Semantic resolution: match a spoken phrase ("the coding monitor", "youtube")
    // against monitor names + the apps living on them. Returns the best monitor, a
    // confidence, and — when two-plus tie — the candidate set for a Clarify.
    SMonitorMatch resolveMonitor(const std::string& phrase) const;

    // Compact, deterministic digest for the intent prompt (bounded by caps).
    std::string digest(int maxMonitors = 16, int maxAppsPerMon = 6) const;
};

// Live snapshot via hyprctl. `runQuery` runs an argv and returns stdout (injectable
// so tests never touch a real compositor). The default runner uses popen(hyprctl).
using QueryFn = std::function<std::string(const std::vector<std::string>& argv)>;
SDesktopContext snapshotDesktop(const QueryFn& runQuery);
std::string     defaultHyprctlQuery(const std::vector<std::string>& argv);
