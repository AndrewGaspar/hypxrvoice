#include "Executor.hpp"
#include "Log.hpp"

#include <spawn.h>
#include <sys/wait.h>
#include <time.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

extern char** environ;

namespace {
    // The closed set of openxr sub-verbs this executor may ever emit. Any argv naming
    // a verb outside this set is rejected by validateStep().
    const std::set<std::string>& allowedOpenxrVerbs() {
        static const std::set<std::string> v = {
            "select", "anchor", "distance", "center", "adaptive",
            "dock", "undock", "roam", "gazegrab", "gazerelease",
            "gazepush", "handinput", "place", "create"};
        return v;
    }

    std::string signedNum(double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s%.2f", v >= 0 ? "+" : "", v);
        return buf;
    }
    std::string plainNum(double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", v);
        return buf;
    }

    void appendEscaped(std::string& out, const std::string& s) {
        out += '"';
        for (char c : s) {
            if (c == '"' || c == '\\')
                out += '\\';
            out += c;
        }
        out += '"';
    }

    // True if `target` denotes the compositor-resolved selection ("active"/empty).
    bool isActive(const std::string& t) { return t.empty() || t == "active"; }

    // A concrete, live monitor name we may `select`. Returns "" if not resolvable.
    std::string concreteName(const SAction& a, const SDesktopContext& ctx) {
        if (isActive(a.target))
            return "";
        return ctx.hasMonitor(a.target) ? a.target : std::string();
    }

    SExecStep step(std::vector<std::string> argv, std::string why) {
        return SExecStep{std::move(argv), std::move(why)};
    }

    // The ONE place an argv learns a 3D point. It reads `gaze.place` — the projected
    // deixis point — and never `gaze.pos`, which is the head origin.
    std::string gazePointArg(const SGazeResolution& g) {
        return plainNum(g.place[0]) + "," + plainNum(g.place[1]) + "," + plainNum(g.place[2]);
    }

    // The shape of a name this daemon is allowed to CREATE: "XR-" + 1..3 digits. It is
    // generated from the live snapshot (SDesktopContext::nextXrMonitorName), never
    // spoken, and this re-check is the executor's own last line of defence.
    bool isCreatableName(const std::string& n) {
        if (n.rfind("XR-", 0) != 0 || n.size() < 4 || n.size() > 6)
            return false;
        for (size_t i = 3; i < n.size(); i++)
            if (!std::isdigit(static_cast<unsigned char>(n[i])))
                return false;
        return true;
    }

    // "WxH@R" with all-numeric fields — the only mode shape we ever emit.
    bool isModeToken(const std::string& m) {
        size_t x = m.find('x'), at = m.find('@');
        if (x == std::string::npos || at == std::string::npos || x == 0 || at < x + 2 || at + 1 >= m.size())
            return false;
        for (size_t i = 0; i < m.size(); i++) {
            if (i == x || i == at)
                continue;
            if (!std::isdigit(static_cast<unsigned char>(m[i])))
                return false;
        }
        return true;
    }

    // "x,y,z" with three plain decimal numbers — the only point shape we ever emit.
    bool isPointToken(const std::string& p) {
        int fields = 1;
        for (char c : p) {
            if (c == ',') { fields++; continue; }
            if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-' && c != '.')
                return false;
        }
        return fields == 3 && p.size() <= 48;
    }
}

std::string SExecPlan::toJson() const {
    std::string o = "{";
    o += "\"ok\":" + std::string(ok ? "true" : "false");
    o += ",\"approximated\":" + std::string(approximated ? "true" : "false");
    o += ",\"reason\":";
    appendEscaped(o, reason);
    o += ",\"steps\":[";
    for (size_t i = 0; i < steps.size(); i++) {
        if (i) o += ',';
        o += "{\"argv\":[";
        for (size_t j = 0; j < steps[i].argv.size(); j++) {
            if (j) o += ',';
            appendEscaped(o, steps[i].argv[j]);
        }
        o += "],\"why\":";
        appendEscaped(o, steps[i].why);
        if (!steps[i].waitForMonitor.empty()) {
            o += ",\"waitFor\":";
            appendEscaped(o, steps[i].waitForMonitor);
        }
        o += "}";
    }
    o += "]}";
    return o;
}

SExecPlan planFor(const SAction& action, const SDesktopContext& ctx, const SExecConfig& cfg) {
    SExecPlan plan;

    // Non-actuating verbs: nothing to run, but not an error — the feedback tier owns
    // the display of Clarify/None.
    if (action.verb == EVerb::None) {
        plan.reason = "out of scope — no compositor command";
        return plan;
    }
    if (action.verb == EVerb::Clarify) {
        plan.reason = action.clarifyQuestion.empty() ? "ambiguous — clarification needed"
                                                      : action.clarifyQuestion;
        return plan;
    }

    if (action.verb == EVerb::LaunchApp) {
        if (!cfg.allowLaunch) {
            plan.reason = "app launch disabled (executor.allow_launch=false)";
            return plan;
        }
        auto it = cfg.appAllowlist.find(action.app);
        if (it == cfg.appAllowlist.end() || it->second.empty()) {
            plan.reason = "app '" + action.app + "' not in launch allowlist — refusing";
            return plan;
        }
        // The allowlist value is a TRUSTED operator string, passed as the single
        // command argument to `dispatch exec`. No transcript text reaches here.
        plan.ok = true;
        plan.steps.push_back(step({"hyprctl", "dispatch", "exec", "--", it->second},
                                  "launch " + action.app));
        return plan;
    }

    // ---- plain window management (round 2) ----
    // These drive Hyprland itself rather than the XR layer, so they sit ahead of the
    // allow_xrmonitor gate and of the monitor-target validation below.
    if (action.verb == EVerb::Workspace || action.verb == EVerb::Focus ||
        action.verb == EVerb::Fullscreen || action.verb == EVerb::MoveWindow ||
        action.verb == EVerb::MoveWorkspace) {
        if (!cfg.allowWindow) {
            plan.reason = "window control disabled (executor.allow_window=false)";
            return plan;
        }
        // "move workspace 4 to this monitor". Both halves are ours: the index is a number
        // WE parsed, the monitor a name enumerated from the live snapshot. There is no
        // window in this verb at all, which is precisely why it exists — round 5's
        // misfire came from a workspace phrase being read as a window reference.
        if (action.verb == EVerb::MoveWorkspace) {
            if (action.workspace < 1 || action.workspace > 99) {
                plan.reason = "workspace index out of range — refusing";
                return plan;
            }
            if (isActive(action.target) || !ctx.hasMonitor(action.target)) {
                plan.reason = "target monitor '" + action.target + "' is not a live monitor — refusing";
                return plan;
            }
            plan.ok = true;
            plan.steps.push_back(step({"hyprctl", "dispatch", "moveworkspacetomonitor",
                                       std::to_string(action.workspace), action.target},
                                      "move workspace " + std::to_string(action.workspace) +
                                          " to " + action.target));
            return plan;
        }
        if (action.verb == EVerb::Workspace) {
            // The index is a number WE parsed and WE format — transcript text never
            // reaches the argv. Out of range is a refusal, not a clamp.
            if (action.workspace < 1 || action.workspace > 99) {
                plan.reason = "workspace index out of range — refusing";
                return plan;
            }
            plan.ok = true;
            plan.steps.push_back(step({"hyprctl", "dispatch", "workspace", std::to_string(action.workspace)},
                                      "switch to workspace " + std::to_string(action.workspace)));
            return plan;
        }

        // MoveWindow: focus the window we are relocating (Hyprland's move dispatchers act
        // on the ACTIVE window), then move it. An empty address means "the focused one",
        // which is exactly what the dispatcher already operates on — so no focus step.
        if (action.verb == EVerb::MoveWindow) {
            if (!action.windowAddress.empty()) {
                if (!ctx.hasWindow(action.windowAddress)) {
                    plan.reason = "window '" + action.windowLabel + "' is not a live window — refusing";
                    return plan;
                }
                plan.steps.push_back(step({"hyprctl", "dispatch", "focuswindow", "address:" + action.windowAddress},
                                          "focus " + (action.windowLabel.empty() ? action.windowAddress
                                                                                 : action.windowLabel)));
            }
            if (action.workspace > 0) {
                if (action.workspace > 99) {
                    plan.reason = "workspace index out of range — refusing";
                    return plan;
                }
                plan.steps.push_back(step({"hyprctl", "dispatch", "movetoworkspace", std::to_string(action.workspace)},
                                          "move to workspace " + std::to_string(action.workspace)));
                plan.ok = true;
                return plan;
            }
            // A monitor destination must be LIVE. `movewindow mon:<name>` relocates the
            // active window onto that output's active workspace.
            if (isActive(action.target) || !ctx.hasMonitor(action.target)) {
                plan.reason = "target monitor '" + action.target + "' is not a live monitor — refusing";
                return plan;
            }
            plan.steps.push_back(step({"hyprctl", "dispatch", "movewindow", "mon:" + action.target},
                                      "move to " + action.target));
            plan.ok = true;
            return plan;
        }

        // Focus/Fullscreen. A named window must still be LIVE in the snapshot — the same
        // rule the monitor verbs follow, so a stale handle can never be dispatched.
        if (!action.windowAddress.empty()) {
            if (!ctx.hasWindow(action.windowAddress)) {
                plan.reason = "window '" + action.windowLabel + "' is not a live window — refusing";
                return plan;
            }
            plan.steps.push_back(step({"hyprctl", "dispatch", "focuswindow", "address:" + action.windowAddress},
                                      "focus " + (action.windowLabel.empty() ? action.windowAddress
                                                                             : action.windowLabel)));
        } else if (action.targetSource == ETargetSource::Deixis && !action.target.empty() &&
                   ctx.hasMonitor(action.target)) {
            // "make THIS window fullscreen" while looking at another monitor: go there
            // first, so "this" means what you are looking at.
            plan.steps.push_back(step({"hyprctl", "dispatch", "focusmonitor", action.target},
                                      "focus " + action.target));
        }

        if (action.verb == EVerb::Fullscreen) {
            // Hyprland: 0 = fullscreen, 1 = maximize. Both toggle, which is the right
            // reading of "make it fullscreen" said twice.
            const bool maximize = action.sub == "maximize";
            plan.steps.push_back(step({"hyprctl", "dispatch", "fullscreen", maximize ? "1" : "0"},
                                      maximize ? "maximize" : "fullscreen"));
        }
        if (plan.steps.empty()) {
            plan.reason = "nothing to focus — no window named and no gaze target";
            return plan;
        }
        plan.ok = true;
        return plan;
    }

    // From here down, every verb actuates the XR layer.
    if (!cfg.allowXrmonitor) {
        plan.reason = "xrmonitor actuation disabled (executor.allow_xrmonitor=false)";
        return plan;
    }

    // "create a monitor here". The one verb whose target is deliberately NOT live yet, so
    // it must sit ahead of the live-target validation below.
    if (action.verb == EVerb::CreateMonitor) {
        if (!cfg.allowCreateMonitor) {
            plan.reason = "monitor creation disabled (executor.allow_create_monitor=false)";
            return plan;
        }
        const std::string newName = action.target;
        if (!isCreatableName(newName)) {
            plan.reason = "generated monitor name '" + newName + "' is not of the form XR-<n> — refusing";
            return plan;
        }
        if (ctx.hasMonitor(newName)) {
            plan.reason = "monitor '" + newName + "' already exists — refusing";
            return plan;
        }
        const std::string mode = ctx.newXrMonitorMode();
        if (!isModeToken(mode)) {
            plan.reason = "generated mode '" + mode + "' is malformed — refusing";
            return plan;
        }
        plan.steps.push_back(step({"hyprctl", "openxr", "create", newName, mode},
                                  "create " + newName + " (" + mode + ")"));
        // "…here": drop the fresh monitor at the PROJECTED gaze point. Without a deixis
        // the compositor's own default placement (in front, at default_distance) stands.
        //
        // The place step WAITS for the monitor it names. `openxr create` returns once the
        // request is accepted, but the output is registered asynchronously — on the live
        // round a create+place pair failed at 21:51 ("step exited 1 — stopping plan") and
        // the identical pair succeeded at 22:07, which is the signature of that race.
        if (action.gaze.valid && action.gaze.placeDistM > 0.0) {
            SExecStep place = step({"hyprctl", "openxr", "place", newName, "at", gazePointArg(action.gaze)},
                                   "place " + newName + " at gaze point");
            place.waitForMonitor = newName;
            plan.steps.push_back(std::move(place));
        }
        plan.ok = true;
        return plan;
    }

    const std::string name = concreteName(action, ctx);
    const bool        active = isActive(action.target);
    // A monitor verb with a named-but-not-live target is refused outright.
    if (!active && name.empty()) {
        plan.reason = "target monitor '" + action.target + "' is not a live monitor — refusing";
        return plan;
    }

    // Helper: prepend a `select <name>` when we have a concrete target (verbs that
    // lack a name argument operate on the current selection).
    auto selectIfNamed = [&](const std::string& why) {
        if (!name.empty())
            plan.steps.push_back(step({"hyprctl", "openxr", "select", name}, "select " + name));
        (void)why;
    };

    switch (action.verb) {
        case EVerb::Pick: {
            if (cfg.caps.targetedGrab && !name.empty()) {
                plan.steps.push_back(step({"hyprctl", "openxr", "gazegrab", name},
                                          "carry " + name + " (targeted grab)"));
            } else {
                // Approximation: select + head-anchor makes the named monitor follow
                // the head immediately, giving the same live-placement feel as a gaze
                // carry, and it is targeted + deterministic (no dependence on live dwell).
                plan.approximated = true;
                plan.reason       = "targeted grab unavailable — using select + anchor head";
                selectIfNamed("");
                std::string t = name.empty() ? "active" : name;
                plan.steps.push_back(step({"hyprctl", "openxr", "anchor", t, "head"},
                                          "pick up " + t + " (head-carry)"));
            }
            plan.ok = true;
            break;
        }
        case EVerb::Place: {
            std::string t = name.empty() ? "active" : name;
            // placeDistM > 0 is the structural proof that the point went through
            // projectPlacePoint(). A hand-built or deserialized SGazeResolution that
            // never did carries 0 and falls through to the freeze-in-place path rather
            // than placing at the LOCAL_FLOOR origin.
            if (cfg.caps.placeAtPose && action.gaze.valid && action.gaze.placeDistM > 0.0) {
                // `place` (the PROJECTED deixis point), NEVER `pos` (the head origin).
                // Passing pos is what dropped a monitor inside the user's head on the
                // first live-fire round — see the SGazeResolution contract.
                const std::string pose = gazePointArg(action.gaze);
                plan.steps.push_back(step({"hyprctl", "openxr", "place", t, "at", pose},
                                          "place " + t + " at gaze point"));
            } else {
                // Approximation: freeze it where it is now. `anchor local` re-anchors
                // without moving the quad — the natural reading of "drop it here".
                plan.approximated = true;
                plan.reason       = "place-at-pose unavailable — freezing in place (anchor local)";
                plan.steps.push_back(step({"hyprctl", "openxr", "anchor", t, "local"},
                                          "drop " + t + " (freeze in place)"));
            }
            plan.ok = true;
            break;
        }
        case EVerb::MoveDist: {
            double d = action.deltaM != 0.0 ? action.deltaM : -cfg.distanceStep;
            selectIfNamed("");
            plan.steps.push_back(step({"hyprctl", "openxr", "distance", signedNum(d)},
                                      d < 0 ? "pull closer" : "push away"));
            plan.ok = true;
            break;
        }
        case EVerb::Center: {
            selectIfNamed("");
            plan.steps.push_back(step({"hyprctl", "openxr", "center"}, "center in view"));
            plan.ok = true;
            break;
        }
        case EVerb::Dock: {
            selectIfNamed("");
            if (action.sub == "here")
                plan.steps.push_back(step({"hyprctl", "openxr", "dock", "here"}, "dock here"));
            else
                plan.steps.push_back(step({"hyprctl", "openxr", "dock"}, "dock"));
            plan.ok = true;
            break;
        }
        case EVerb::Undock: {
            selectIfNamed("");
            plan.steps.push_back(step({"hyprctl", "openxr", "undock"}, "undock / follow now"));
            plan.ok = true;
            break;
        }
        case EVerb::Follow: {
            selectIfNamed("");
            plan.steps.push_back(step({"hyprctl", "openxr", "adaptive", "on"}, "follow me (adaptive on)"));
            // Optional roam mode from the anchor hint or sub-action.
            std::string roam;
            if (action.anchor == EAnchorMode::Head) roam = "head";
            else if (action.anchor == EAnchorMode::Body) roam = "body";
            else if (action.sub == "head" || action.sub == "body") roam = action.sub;
            if (!roam.empty())
                plan.steps.push_back(step({"hyprctl", "openxr", "roam", roam}, "roam " + roam));
            plan.ok = true;
            break;
        }
        case EVerb::Anchor: {
            if (action.anchor == EAnchorMode::Unset) {
                plan.reason = "anchor mode unspecified — refusing";
                return plan;
            }
            std::string t = name.empty() ? "active" : name;
            plan.steps.push_back(step({"hyprctl", "openxr", "anchor", t, anchorModeName(action.anchor)},
                                      std::string("anchor ") + anchorModeName(action.anchor)));
            plan.ok = true;
            break;
        }
        case EVerb::HandInput: {
            std::string s = action.sub.empty() ? "toggle" : action.sub;
            if (s != "on" && s != "off" && s != "auto" && s != "toggle") {
                plan.reason = "hand input sub-action must be on|off|auto|toggle";
                return plan;
            }
            plan.steps.push_back(step({"hyprctl", "openxr", "handinput", s}, "hand input " + s));
            plan.ok = true;
            break;
        }
        default:
            plan.reason = "verb not executable";
            return plan;
    }

    return plan;
}

bool validateStep(const SExecStep& step, std::string& err) {
    const auto& a = step.argv;
    if (a.empty() || a[0] != "hyprctl") {
        err = "step does not begin with hyprctl";
        return false;
    }
    if (a.size() < 2) {
        err = "step too short";
        return false;
    }
    if (a[1] == "openxr") {
        if (a.size() < 3) {
            err = "openxr step missing verb";
            return false;
        }
        if (!allowedOpenxrVerbs().count(a[2])) {
            err = "openxr verb '" + a[2] + "' not in allowlist";
            return false;
        }
        // `create` and `place` carry data (a name we minted, a pixel mode, a 3D point),
        // so their SHAPES are checked here too — the same treatment the dispatchers get.
        if (a[2] == "create") {
            if (a.size() != 5 || !isCreatableName(a[3]) || !isModeToken(a[4])) {
                err = "create must be `openxr create XR-<n> <WxH@Hz>`";
                return false;
            }
        }
        if (a[2] == "place") {
            if (a.size() != 6 || a[4] != "at" || !isPointToken(a[5])) {
                err = "place must be `openxr place <name> at <x,y,z>`";
                return false;
            }
        }
        // Reject any token that could smuggle shell/meta — we spawn without a shell,
        // but keep argv clean regardless. No newlines/NULs; names/modes/numbers only.
        for (auto& tok : a)
            for (char c : tok)
                if (c == '\n' || c == '\r' || c == '\0') {
                    err = "argv token contains a control character";
                    return false;
                }
        return true;
    }
    if (a[1] == "dispatch") {
        if (a.size() < 3) {
            err = "dispatch step missing a dispatcher";
            return false;
        }
        // A CLOSED set of dispatchers, each with its argument SHAPE checked here. This is
        // the last line of defence: even if a bug upstream let a spoken word reach an
        // argv, none of these shapes can carry one.
        if (a[2] == "exec") {
            if (a.size() < 5 || a[3] != "--") {
                err = "dispatch step must be `dispatch exec -- <cmd>`";
                return false;
            }
            return true;
        }
        if (a[2] == "focuswindow") {
            // `address:0x…` only. NOT `class:…` — Hyprland reads that as a REGEX, so a
            // class carrying a metachar could select the wrong window, or none.
            if (a.size() != 4 || a[3].rfind("address:0x", 0) != 0 || a[3].size() <= 10 ||
                a[3].size() > 10 + 16) {
                err = "focuswindow must be `focuswindow address:0x<hex>`";
                return false;
            }
            for (size_t i = 10; i < a[3].size(); i++)
                if (!std::isxdigit(static_cast<unsigned char>(a[3][i]))) {
                    err = "focuswindow address is not hexadecimal";
                    return false;
                }
            return true;
        }
        if (a[2] == "focusmonitor") {
            if (a.size() != 4 || a[3].empty() || a[3].size() > 64) {
                err = "focusmonitor must be `focusmonitor <name>`";
                return false;
            }
            // Monitor names come from the live snapshot; keep the charset boring anyway.
            for (char c : a[3])
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') {
                    err = "focusmonitor name has an unexpected character";
                    return false;
                }
            return true;
        }
        if (a[2] == "fullscreen") {
            if (a.size() != 4 || (a[3] != "0" && a[3] != "1")) {
                err = "fullscreen must be `fullscreen 0|1`";
                return false;
            }
            return true;
        }
        if (a[2] == "movewindow") {
            // `movewindow mon:<name>` ONLY. The directional form is not something this
            // grammar can produce, and refusing it here keeps the shape trivially checkable.
            if (a.size() != 4 || a[3].rfind("mon:", 0) != 0 || a[3].size() <= 4 || a[3].size() > 4 + 64) {
                err = "movewindow must be `movewindow mon:<name>`";
                return false;
            }
            for (size_t i = 4; i < a[3].size(); i++) {
                const char c = a[3][i];
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') {
                    err = "movewindow monitor name has an unexpected character";
                    return false;
                }
            }
            return true;
        }
        if (a[2] == "moveworkspacetomonitor") {
            // `moveworkspacetomonitor <1-99> <name>` — hyprctl joins the argv with
            // spaces, which is exactly the two-token form the dispatcher parses
            // (Hyprland src/config/legacy/DispatcherTranslator.cpp).
            if (a.size() != 5 || a[3].empty() || a[3].size() > 2 || a[4].empty() || a[4].size() > 64) {
                err = "moveworkspacetomonitor must be `moveworkspacetomonitor <1-99> <name>`";
                return false;
            }
            for (char c : a[3])
                if (!std::isdigit(static_cast<unsigned char>(c))) {
                    err = "workspace index is not a number";
                    return false;
                }
            if (a[3] == "0" || a[3] == "00") {
                err = "workspace index must be >= 1";
                return false;
            }
            for (char c : a[4])
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') {
                    err = "moveworkspacetomonitor name has an unexpected character";
                    return false;
                }
            return true;
        }
        if (a[2] == "movetoworkspace" || a[2] == "workspace") {
            if (a.size() != 4 || a[3].empty() || a[3].size() > 2) {
                err = a[2] + " must be `" + a[2] + " <1-99>`";
                return false;
            }
            for (char c : a[3])
                if (!std::isdigit(static_cast<unsigned char>(c))) {
                    err = "workspace index is not a number";
                    return false;
                }
            if (a[3] == "0" || a[3] == "00") {
                err = "workspace index must be >= 1";
                return false;
            }
            return true;
        }
        err = "dispatcher '" + a[2] + "' not in allowlist";
        return false;
    }
    err = "second token '" + a[1] + "' not in {openxr, dispatch}";
    return false;
}

int defaultRunner(const std::vector<std::string>& argv) {
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (auto& s : argv)
        cargv.push_back(const_cast<char*>(s.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = -1;
    int   rc  = posix_spawnp(&pid, cargv[0], nullptr, nullptr, cargv.data(), environ);
    if (rc != 0)
        return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool defaultMonitorProbe(const std::string& name) {
    // Read-only, and only the one query the precondition needs.
    const std::string mons = defaultHyprctlQuery({"hyprctl", "monitors", "-j"});
    return SDesktopContext::parse(mons, "", "").hasMonitor(name);
}

void defaultSleep(int ms) {
    if (ms <= 0)
        return;
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}

namespace {
    // Poll until `name` is a live monitor, or the budget runs out. One probe always
    // happens first: when the compositor already registered the output — the common
    // case — nothing is slept at all.
    bool awaitMonitor(const std::string& name, const SExecConfig& cfg,
                      const MonitorProbeFn& probe, const SleepFn& nap) {
        const MonitorProbeFn p = probe ? probe : MonitorProbeFn(defaultMonitorProbe);
        const SleepFn        n = nap ? nap : SleepFn(defaultSleep);
        const int            poll = std::max(1, cfg.waitPollMs);
        int                  left = std::max(0, cfg.waitMonitorMs);
        for (;;) {
            if (p(name))
                return true;
            if (left <= 0)
                return false;
            const int slice = std::min(poll, left);
            n(slice);
            left -= slice;
        }
    }
}

int runPlan(const SExecPlan& plan, const SExecConfig& cfg, const RunFn& run,
            const MonitorProbeFn& probe, const SleepFn& nap) {
    if (!plan.ok) {
        Log::log(Log::INFO, "executor: nothing to actuate ({})", plan.reason);
        return 0;
    }
    int dispatched = 0;
    for (auto& s : plan.steps) {
        std::string err;
        if (!validateStep(s, err)) {
            Log::log(Log::ERR, "executor: refusing step — {}", err);
            return dispatched; // hard stop
        }
        std::string joined;
        for (auto& t : s.argv) {
            if (!joined.empty()) joined += ' ';
            joined += t;
        }
        if (cfg.dryRun) {
            if (!s.waitForMonitor.empty())
                Log::log(Log::INFO, "executor[dry-run]: (would wait for monitor {})", s.waitForMonitor);
            Log::log(Log::INFO, "executor[dry-run]: {}  # {}", joined, s.why);
            continue;
        }
        // Precondition: the monitor this step names must exist before we dispatch at it.
        if (!s.waitForMonitor.empty() && !awaitMonitor(s.waitForMonitor, cfg, probe, nap)) {
            Log::log(Log::ERR, "executor: monitor '{}' did not appear within {} ms — stopping plan",
                     s.waitForMonitor, cfg.waitMonitorMs);
            return dispatched;
        }
        Log::log(Log::INFO, "executor: {}  # {}", joined, s.why);
        int rc = run(s.argv);
        dispatched++;
        if (rc != 0) {
            Log::log(Log::WARN, "executor: step exited {} — stopping plan", rc);
            break;
        }
    }
    return dispatched;
}
