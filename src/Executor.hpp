#pragma once

#include "Command.hpp"
#include "DesktopContext.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

// WP-V4 command executor: a PURE mapping from a typed SAction to a plan of allowlisted
// `hyprctl` argv vectors, plus a runner that executes (or, in dry-run, logs) them.
//
// SAFETY CONTRACT (non-negotiable):
//   * Transcript text is NEVER interpolated into a command line. Every argv token is
//     either a constant, a monitor name validated against the live snapshot, a mode
//     from a closed enum, a number WE format, or an app command from the operator's
//     allowlist. validateStep() enforces this as a last line of defence before run.
//   * The plan is built without side effects; execution is a separate, explicit step.
//   * dry_run logs the exact argv and actuates nothing — this is how the pipeline is
//     tested and the default in the shipped example config.

// Optional compositor verbs the daemon can use if the running compositor advertises
// them (see docs/COMPOSITOR-GAPS.md). Off by default: the executor falls back to a
// documented approximation built from verbs that exist today.
struct SCapabilities {
    bool targetedGrab = false; // `openxr gazegrab <name>` — grab a NAMED monitor into a carry.
    bool placeAtPose  = false; // `openxr place <name> at x,y,z` — drop at a resolved gaze pose.
};

struct SExecConfig {
    bool dryRun        = true;  // DEFAULT: log argv, actuate nothing.
    bool allowXrmonitor = true; // master switch for all openxr/xrmonitor actuation.
    bool allowLaunch   = false; // app launch is off unless explicitly enabled.
    // Plain window management (focus / fullscreen / workspace). Non-destructive and
    // reversible — nothing in this class can close, kill, or relocate a window — so it
    // is permitted by default; dryRun still decides whether anything actually runs.
    bool allowWindow   = true;
    // "create a monitor here". Additive and individually undoable (`hyprctl openxr
    // destroy`), and runtime-created monitors are never touched by config
    // reconciliation, so it rides with the rest of the XR verbs under allow_xrmonitor —
    // this is the finer-grained off switch.
    bool allowCreateMonitor = true;
    double distanceStep = 0.25; // default push/pull step (m) when the action gives none.
    // Allowlist: spoken app key -> a TRUSTED launch command (operator config). The
    // command is passed to `hyprctl dispatch exec -- <cmd>` verbatim; it is never
    // derived from transcript text. Use the uwsm/systemd-run pattern, e.g.
    //   "browser" -> "uwsm app -- firefox.desktop".
    std::map<std::string, std::string> appAllowlist;
    SCapabilities caps;
};

struct SExecStep {
    std::vector<std::string> argv; // e.g. {"hyprctl","openxr","anchor","XR-code","local"}
    std::string              why;  // short human note for logs/HUD.
};

struct SExecPlan {
    bool                    ok = false; // false => refused/unsupported; nothing to run.
    std::string             reason;     // refusal cause or an informational note.
    bool                    approximated = false; // used a fallback for a missing capability.
    std::vector<SExecStep>  steps;

    std::string toJson() const;
};

// Build the plan for an action against the live snapshot. Pure. A concrete monitor
// target that is not live in `ctx` is REFUSED (never actuate on an invented name).
SExecPlan planFor(const SAction& action, const SDesktopContext& ctx, const SExecConfig& cfg);

// Validate a single step against the hard allowlist. Returns false (with `err`) if the
// step is anything other than a permitted `hyprctl openxr <verb> …` / `hyprctl dispatch
// exec -- …` shape. Called by run() before every step; exposed for unit tests.
bool validateStep(const SExecStep& step, std::string& err);

// The process runner: given an argv, spawn it and return the exit status. Injectable
// so tests record argv without spawning. The default spawns hyprctl via posix_spawnp
// (no shell — no injection surface).
using RunFn = std::function<int(const std::vector<std::string>& argv)>;
int  defaultRunner(const std::vector<std::string>& argv);

// Execute a plan. In dry_run, logs each validated step and runs nothing. Otherwise
// validates then runs each step in order, stopping on the first failure. Returns the
// number of steps actually dispatched (0 in dry-run). Any step failing validation is a
// hard stop (logged) — defence in depth.
int runPlan(const SExecPlan& plan, const SExecConfig& cfg, const RunFn& run);
