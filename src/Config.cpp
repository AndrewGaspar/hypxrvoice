#include "Config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {
    std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos)
            return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    // Strip a trailing unquoted `# comment`. Respects a quoted string so a '#'
    // inside quotes is preserved.
    std::string stripComment(const std::string& s) {
        bool inStr = false;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '"')
                inStr = !inStr;
            else if (s[i] == '#' && !inStr)
                return s.substr(0, i);
        }
        return s;
    }

    // Unquote a "..." string value; returns false if not a quoted string.
    bool asString(const std::string& v, std::string& out) {
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
            out.clear();
            for (size_t i = 1; i + 1 < v.size(); i++) {
                if (v[i] == '\\' && i + 2 < v.size()) {
                    char n = v[++i];
                    switch (n) {
                        case 'n': out += '\n'; break;
                        case 't': out += '\t'; break;
                        case '"': out += '"'; break;
                        case '\\': out += '\\'; break;
                        default: out += n;
                    }
                } else {
                    out += v[i];
                }
            }
            return true;
        }
        return false;
    }

    bool asBool(const std::string& v, bool& out) {
        if (v == "true") { out = true; return true; }
        if (v == "false") { out = false; return true; }
        return false;
    }

    bool asInt(const std::string& v, long& out) {
        if (v.empty())
            return false;
        char*     end = nullptr;
        long      r   = std::strtol(v.c_str(), &end, 10);
        if (end == v.c_str() || *end != '\0')
            return false;
        out = r;
        return true;
    }

    bool asFloat(const std::string& v, double& out) {
        if (v.empty())
            return false;
        char*  end = nullptr;
        double r   = std::strtod(v.c_str(), &end);
        if (end == v.c_str() || *end != '\0')
            return false;
        out = r;
        return true;
    }
}

bool parseConfig(const std::string& text, SConfig& out, std::vector<std::string>& errors, std::vector<std::string>& warnings) {
    std::istringstream in(text);
    std::string        line;
    std::string        section;
    int                lineNo = 0;

    auto err  = [&](const std::string& m) { errors.push_back("line " + std::to_string(lineNo) + ": " + m); };
    auto warn = [&](const std::string& m) { warnings.push_back("line " + std::to_string(lineNo) + ": " + m); };

    // Helper macros to reduce boilerplate for each typed field.
    auto setStr = [&](std::string& dst, const std::string& v) {
        std::string s;
        if (asString(v, s)) { dst = s; return true; }
        err("expected a quoted string");
        return false;
    };
    auto setInt = [&](int& dst, const std::string& v) {
        long l;
        if (asInt(v, l)) { dst = static_cast<int>(l); return true; }
        err("expected an integer");
        return false;
    };
    auto setFloat = [&](float& dst, const std::string& v) {
        double d;
        if (asFloat(v, d)) { dst = static_cast<float>(d); return true; }
        err("expected a number");
        return false;
    };
    auto setDouble = [&](double& dst, const std::string& v) {
        double d;
        if (asFloat(v, d)) { dst = d; return true; }
        err("expected a number");
        return false;
    };
    auto setBool = [&](bool& dst, const std::string& v) {
        bool b;
        if (asBool(v, b)) { dst = b; return true; }
        err("expected true/false");
        return false;
    };

    while (std::getline(in, line)) {
        lineNo++;
        std::string s = trim(stripComment(line));
        if (s.empty())
            continue;

        if (s.front() == '[') {
            if (s.back() != ']') {
                err("malformed section header");
                continue;
            }
            section = trim(s.substr(1, s.size() - 2));
            continue;
        }

        size_t eq = s.find('=');
        if (eq == std::string::npos) {
            err("expected key = value");
            continue;
        }
        std::string key = trim(s.substr(0, eq));
        std::string val = trim(s.substr(eq + 1));
        std::string k   = section + "." + key;

        // Dispatch on fully-qualified key.
        if (k == "activation.mode") {
            std::string v;
            if (!setStr(v, val)) continue;
            if (v == "auto") out.activation.mode = EActivationMode::Auto;
            else if (v == "ptt") out.activation.mode = EActivationMode::Ptt;
            else if (v == "wake") out.activation.mode = EActivationMode::Wake;
            else err("activation.mode must be auto|ptt|wake");
        } else if (k == "activation.fallback") {
            std::string v;
            if (!setStr(v, val)) continue;
            if (v == "wake") out.activation.fallback = EFallback::Wake;
            else if (v == "gate") out.activation.fallback = EFallback::Gate;
            else err("activation.fallback must be wake|gate");
        } else if (k == "activation.keyboard_idle_ms") {
            setInt(out.activation.keyboardIdleMs, val);
        } else if (k == "audio.source") {
            setStr(out.audio.source, val);
        } else if (k == "audio.headset_source") {
            setStr(out.audio.headsetSource, val);
        } else if (k == "audio.sample_rate") {
            setInt(out.audio.sampleRate, val);
        } else if (k == "vad.energy_threshold") {
            setFloat(out.vad.energyThreshold, val);
        } else if (k == "vad.start_ms") {
            setInt(out.vad.startMs, val);
        } else if (k == "vad.end_ms") {
            setInt(out.vad.endMs, val);
        } else if (k == "vad.max_utterance_ms") {
            setInt(out.vad.maxUtteranceMs, val);
        } else if (k == "vad.pre_roll_ms") {
            setInt(out.vad.preRollMs, val);
        } else if (k == "wake.enabled") {
            setBool(out.wake.enabled, val);
        } else if (k == "wake.backend") {
            setStr(out.wake.backend, val);
        } else if (k == "wake.phrase") {
            setStr(out.wake.phrase, val);
        } else if (k == "wake.fuzz") {
            setInt(out.wake.fuzz, val);
        } else if (k == "asr.model") {
            setStr(out.asr.model, val);
        } else if (k == "asr.language") {
            setStr(out.asr.language, val);
        } else if (k == "asr.threads") {
            setInt(out.asr.threads, val);
        } else if (k == "asr.translate") {
            setBool(out.asr.translate, val);
        } else if (k == "feedback.stdout_json") {
            setBool(out.feedback.stdoutJson, val);
        } else if (k == "feedback.notify") {
            setBool(out.feedback.notify, val);
        } else if (k == "compositor.enabled") {
            setBool(out.compositor.enabled, val);
        } else if (k == "compositor.poll_ms") {
            setInt(out.compositor.pollMs, val);
        } else if (k == "intent.enabled") {
            setBool(out.intent.enabled, val);
        } else if (k == "intent.backend") {
            setStr(out.intent.backend, val);
        } else if (k == "intent.model") {
            setStr(out.intent.model, val);
        } else if (k == "intent.temperature") {
            setDouble(out.intent.temperature, val);
        } else if (k == "intent.threads") {
            setInt(out.intent.nThreads, val);
        } else if (k == "intent.context_max_monitors") {
            setInt(out.intent.contextMaxMonitors, val);
        } else if (k == "intent.context_max_apps") {
            setInt(out.intent.contextMaxApps, val);
        } else if (k == "intent.deixis_window_ms") {
            setInt(out.intent.deixisWindowMs, val);
        } else if (k == "intent.deixis_lead_ms") {
            setInt(out.intent.deixisLeadMs, val);
        } else if (k == "intent.deixis_samples") {
            setInt(out.intent.deixisSamples, val);
        } else if (k == "intent.distance_step_m") {
            setDouble(out.intent.distanceStep, val);
        } else if (k == "executor.dry_run") {
            setBool(out.executor.dryRun, val);
        } else if (k == "executor.allow_xrmonitor") {
            setBool(out.executor.allowXrmonitor, val);
        } else if (k == "executor.allow_launch") {
            setBool(out.executor.allowLaunch, val);
        } else if (k == "executor.targeted_grab") {
            setBool(out.executor.targetedGrab, val);
        } else if (k == "executor.place_at_pose") {
            setBool(out.executor.placeAtPose, val);
        } else if (section == "apps") {
            // [apps] allowlist: any key is an app alias mapped to a trusted command.
            std::string s;
            if (asString(val, s))
                out.apps[key] = s;
            else
                err("app allowlist value must be a quoted command string");
        } else {
            warn("unknown key '" + k + "' (ignored)");
        }
    }

    return errors.empty();
}

bool loadConfigFile(const std::string& path, SConfig& out, std::vector<std::string>& errors, std::vector<std::string>& warnings) {
    std::ifstream f(path);
    if (!f) {
        warnings.push_back("config file '" + path + "' not found; using defaults");
        return true;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return parseConfig(ss.str(), out, errors, warnings);
}

std::string defaultConfigPath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::string(xdg) + "/hypxrvoice/config.toml";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.config/hypxrvoice/config.toml";
    return "config.toml";
}
