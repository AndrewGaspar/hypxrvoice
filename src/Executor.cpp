#include "Executor.hpp"
#include "Log.hpp"

#include <spawn.h>
#include <sys/wait.h>

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
            "gazepush", "handinput", "place"};
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
        action.verb == EVerb::Fullscreen || action.verb == EVerb::MoveWindow) {
        if (!cfg.allowWindow) {
            plan.reason = "window control disabled (executor.allow_window=false)";
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
            if (cfg.caps.placeAtPose && action.gaze.valid) {
                std::string pose = plainNum(action.gaze.pos[0]) + "," +
                                   plainNum(action.gaze.pos[1]) + "," +
                                   plainNum(action.gaze.pos[2]);
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

int runPlan(const SExecPlan& plan, const SExecConfig& cfg, const RunFn& run) {
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
            Log::log(Log::INFO, "executor[dry-run]: {}  # {}", joined, s.why);
            continue;
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
