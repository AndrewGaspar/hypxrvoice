#include "Command.hpp"

#include <jansson.h>

#include <cstdio>
#include <cstring>

const char* verbName(EVerb v) {
    switch (v) {
        case EVerb::None:      return "none";
        case EVerb::Clarify:   return "clarify";
        case EVerb::Pick:      return "pick";
        case EVerb::Place:     return "place";
        case EVerb::MoveDist:  return "move_dist";
        case EVerb::Center:    return "center";
        case EVerb::Dock:      return "dock";
        case EVerb::Undock:    return "undock";
        case EVerb::Follow:    return "follow";
        case EVerb::Anchor:    return "anchor";
        case EVerb::HandInput: return "hand_input";
        case EVerb::LaunchApp: return "launch_app";
    }
    return "?";
}

EVerb verbFromName(const std::string& s) {
    if (s == "none")       return EVerb::None;
    if (s == "clarify")    return EVerb::Clarify;
    if (s == "pick")       return EVerb::Pick;
    if (s == "place")      return EVerb::Place;
    if (s == "move_dist")  return EVerb::MoveDist;
    if (s == "center")     return EVerb::Center;
    if (s == "dock")       return EVerb::Dock;
    if (s == "undock")     return EVerb::Undock;
    if (s == "follow")     return EVerb::Follow;
    if (s == "anchor")     return EVerb::Anchor;
    if (s == "hand_input") return EVerb::HandInput;
    if (s == "launch_app") return EVerb::LaunchApp;
    return EVerb::None;
}

const char* anchorModeName(EAnchorMode m) {
    switch (m) {
        case EAnchorMode::Unset:       return "";
        case EAnchorMode::Local:       return "local";
        case EAnchorMode::Head:        return "head";
        case EAnchorMode::Body:        return "body";
        case EAnchorMode::DeviceLeft:  return "device:left";
        case EAnchorMode::DeviceRight: return "device:right";
    }
    return "";
}

EAnchorMode parseAnchorMode(const std::string& s) {
    if (s == "local")        return EAnchorMode::Local;
    if (s == "head")         return EAnchorMode::Head;
    if (s == "body")         return EAnchorMode::Body;
    if (s == "device:left")  return EAnchorMode::DeviceLeft;
    if (s == "device:right") return EAnchorMode::DeviceRight;
    return EAnchorMode::Unset;
}

const char* targetSourceName(ETargetSource s) {
    switch (s) {
        case ETargetSource::None:     return "none";
        case ETargetSource::Active:   return "active";
        case ETargetSource::Named:    return "named";
        case ETargetSource::Semantic: return "semantic";
        case ETargetSource::Deixis:   return "deixis";
    }
    return "?";
}

namespace {
    void appendEscaped(std::string& out, const std::string& s) {
        out += '"';
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        out += '"';
    }

    std::string num(double v, const char* fmt = "%.4f") {
        char buf[48];
        std::snprintf(buf, sizeof(buf), fmt, v);
        return buf;
    }
}

std::string SGazeResolution::toJson() const {
    std::string o = "{";
    o += "\"valid\":" + std::string(valid ? "true" : "false");
    o += ",\"monitorId\":" + std::to_string(monitorId);
    o += ",\"name\":";
    appendEscaped(o, name);
    o += ",\"pos\":[" + num(pos[0]) + "," + num(pos[1]) + "," + num(pos[2]) + "]";
    o += ",\"forward\":[" + num(forward[0]) + "," + num(forward[1]) + "," + num(forward[2]) + "]";
    o += ",\"dwellSec\":" + num(dwellSec, "%.3f");
    o += ",\"matchedMs\":" + std::to_string(matchedMs);
    o += ",\"requestedMs\":" + std::to_string(requestedMs);
    o += ",\"ageMs\":" + std::to_string(ageMs);
    o += ",\"stable\":" + std::string(stable ? "true" : "false");
    o += ",\"agree\":" + std::to_string(agreeCount) + "/" + std::to_string(sampleCount);
    o += "}";
    return o;
}

std::string SAction::toJson() const {
    std::string o = "{";
    o += "\"verb\":\"" + std::string(verbName(verb)) + "\"";
    if (!target.empty()) {
        o += ",\"target\":";
        appendEscaped(o, target);
        o += ",\"targetSource\":\"" + std::string(targetSourceName(targetSource)) + "\"";
    }
    if (anchor != EAnchorMode::Unset)
        o += ",\"anchor\":\"" + std::string(anchorModeName(anchor)) + "\"";
    if (!sub.empty()) {
        o += ",\"sub\":";
        appendEscaped(o, sub);
    }
    if (verb == EVerb::MoveDist)
        o += ",\"deltaM\":" + num(deltaM, "%.3f");
    if (!app.empty()) {
        o += ",\"app\":";
        appendEscaped(o, app);
    }
    o += ",\"confidence\":" + num(confidence, "%.2f");
    if (verb == EVerb::Clarify) {
        o += ",\"clarifyQuestion\":";
        appendEscaped(o, clarifyQuestion);
        o += ",\"clarifyCandidates\":[";
        for (size_t i = 0; i < clarifyCandidates.size(); i++) {
            if (i) o += ',';
            appendEscaped(o, clarifyCandidates[i]);
        }
        o += "]";
    }
    if (gaze.valid || targetSource == ETargetSource::Deixis)
        o += ",\"gaze\":" + gaze.toJson();
    if (!note.empty()) {
        o += ",\"note\":";
        appendEscaped(o, note);
    }
    if (!utterance.empty()) {
        o += ",\"utterance\":";
        appendEscaped(o, utterance);
    }
    o += "}";
    return o;
}

namespace {
    std::string jstr(json_t* obj, const char* key) {
        json_t* v = json_object_get(obj, key);
        return (v && json_is_string(v)) ? json_string_value(v) : "";
    }
    bool has(json_t* obj, const char* key) { return json_object_get(obj, key) != nullptr; }
    double jnum(json_t* obj, const char* key, double def) {
        json_t* v = json_object_get(obj, key);
        if (v && json_is_number(v)) return json_number_value(v);
        return def;
    }
}

bool parseAction(const std::string& json, SAction& out, std::string& err) {
    json_error_t jerr{};
    json_t*      root = json_loads(json.c_str(), 0, &jerr);
    if (!root) {
        err = std::string("intent JSON parse error: ") + jerr.text;
        return false;
    }
    if (!json_is_object(root)) {
        json_decref(root);
        err = "intent JSON is not an object";
        return false;
    }

    out = SAction{};
    out.verb = verbFromName(jstr(root, "verb"));

    out.target = jstr(root, "target");
    std::string ts = jstr(root, "targetSource");
    if      (ts == "active")   out.targetSource = ETargetSource::Active;
    else if (ts == "named")    out.targetSource = ETargetSource::Named;
    else if (ts == "semantic") out.targetSource = ETargetSource::Semantic;
    else if (ts == "deixis")   out.targetSource = ETargetSource::Deixis;
    else if (!out.target.empty()) out.targetSource = ETargetSource::Named; // sensible default

    if (has(root, "anchor"))
        out.anchor = parseAnchorMode(jstr(root, "anchor"));
    out.sub = jstr(root, "sub");
    out.deltaM = jnum(root, "deltaM", 0.0);
    out.app = jstr(root, "app");
    out.confidence = jnum(root, "confidence", 1.0);
    out.clarifyQuestion = jstr(root, "clarifyQuestion");
    if (json_t* cands = json_object_get(root, "clarifyCandidates"); cands && json_is_array(cands)) {
        size_t  i;
        json_t* el;
        json_array_foreach(cands, i, el) {
            if (json_is_string(el)) out.clarifyCandidates.emplace_back(json_string_value(el));
        }
    }
    out.note = jstr(root, "note");

    json_decref(root);
    return true;
}
