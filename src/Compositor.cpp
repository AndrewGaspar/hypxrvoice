#include "Compositor.hpp"
#include "Log.hpp"

#include <jansson.h>

#include <array>
#include <cstdio>
#include <memory>
#include <string>

SEnvSignal CCompositor::parseStatus(const std::string& json) {
    SEnvSignal env; // defaults: compositorAvailable=false, atKeyboard=true
    if (json.empty())
        return env;

    json_error_t jerr{};
    json_t*      root = json_loads(json.c_str(), 0, &jerr);
    if (!root)
        return env;

    // We require at least one of the openxr fields to consider the section present.
    json_t* presence = json_object_get(root, "userPresence");
    json_t* visible  = json_object_get(root, "visible");
    json_t* handIn   = json_object_get(root, "handInput");

    auto str = [](json_t* v) -> std::string {
        return (v && json_is_string(v)) ? json_string_value(v) : "";
    };

    const std::string sPresence = str(presence);
    const std::string sVisible  = str(visible);
    std::string       sHandState;
    if (handIn && json_is_object(handIn))
        sHandState = str(json_object_get(handIn, "state"));

    if (sPresence.empty() && sVisible.empty() && sHandState.empty()) {
        json_decref(root);
        return env; // no openxr section — treat as unavailable
    }

    env.compositorAvailable = true;

    // Presence: prefer the explicit user-presence signal; fall back to raw
    // visibility when presence is unknown/unsupported.
    if (sPresence == "yes")
        env.headsetPresent = true;
    else if (sPresence == "no")
        env.headsetPresent = false;
    else
        env.headsetPresent = (sVisible == "yes");

    // At-keyboard: only "active" definitively means away from the keyboard. Any
    // gated/off/unknown state stays conservative (at-keyboard => PTT-gated).
    env.atKeyboard = (sHandState != "active");

    json_decref(root);
    return env;
}

SEnvSignal CCompositor::poll(std::string* rawJson) {
    // hyprctl inherits HYPRLAND_INSTANCE_SIGNATURE from our environment. 2>/dev/null
    // suppresses its "couldn't connect" chatter on stderr when no compositor is up.
    std::string cmd = "hyprctl openxr status -j 2>/dev/null";
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
    std::string out;
    if (pipe) {
        std::array<char, 4096> buf{};
        size_t                 n;
        while ((n = fread(buf.data(), 1, buf.size(), pipe.get())) > 0)
            out.append(buf.data(), n);
    }
    if (rawJson)
        *rawJson = out;

    SEnvSignal env = parseStatus(out);
    if (!env.compositorAvailable && !m_warnedUnavailable) {
        Log::log(Log::WARN, "compositor openxr status unavailable; applying activation fallback");
        m_warnedUnavailable = true;
    } else if (env.compositorAvailable && m_warnedUnavailable) {
        Log::log(Log::INFO, "compositor openxr status available again");
        m_warnedUnavailable = false;
    }
    return env;
}
