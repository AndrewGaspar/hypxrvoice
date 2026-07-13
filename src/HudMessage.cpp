#include "HudMessage.hpp"

#include <jansson.h>

namespace {
    const char* colorName(EHudColor c) {
        switch (c) {
            case EHudColor::Normal: return "normal";
            case EHudColor::Dim:    return "dim";
            case EHudColor::Accent: return "accent";
            case EHudColor::Good:   return "good";
            case EHudColor::Warn:   return "warn";
            case EHudColor::Bad:    return "bad";
        }
        return "normal";
    }
    EHudColor colorFromName(const char* s) {
        if (!s) return EHudColor::Normal;
        std::string n = s;
        if (n == "dim")    return EHudColor::Dim;
        if (n == "accent") return EHudColor::Accent;
        if (n == "good")   return EHudColor::Good;
        if (n == "warn")   return EHudColor::Warn;
        if (n == "bad")    return EHudColor::Bad;
        return EHudColor::Normal;
    }
    EHudState stateFromName(const char* s) {
        if (!s) return EHudState::Hidden;
        std::string n = s;
        if (n == "listening") return EHudState::Listening;
        if (n == "action")    return EHudState::Action;
        if (n == "clarify")   return EHudState::Clarify;
        if (n == "error")     return EHudState::Error;
        return EHudState::Hidden;
    }
}

namespace HudMsg {
    std::string serialize(const SHudView& v) {
        json_t* root = json_object();
        json_object_set_new(root, "state", json_string(hudStateName(v.state)));
        json_t* lines = json_array();
        for (const auto& ln : v.lines) {
            json_t* o = json_object();
            json_object_set_new(o, "t", json_string(ln.text.c_str()));
            json_object_set_new(o, "c", json_string(colorName(ln.color)));
            json_object_set_new(o, "big", json_boolean(ln.big));
            json_array_append_new(lines, o);
        }
        json_object_set_new(root, "lines", lines);
        json_object_set_new(root, "conf", json_real(v.confidence));
        json_object_set_new(root, "approx", json_boolean(v.approximated));
        json_object_set_new(root, "dry", json_boolean(v.dryRun));
        json_object_set_new(root, "rise", json_integer(v.riseMs));
        json_object_set_new(root, "hold", json_integer(v.holdMs));
        json_object_set_new(root, "fade", json_integer(v.fadeMs));
        json_object_set_new(root, "ceil", json_real(v.opacityCeil));

        char* s = json_dumps(root, JSON_COMPACT);
        std::string out = s ? s : "{}";
        free(s);
        json_decref(root);
        out += '\n';
        return out;
    }

    bool parse(const std::string& line, SHudView& out) {
        json_error_t err;
        json_t*      root = json_loads(line.c_str(), 0, &err);
        if (!root || !json_is_object(root)) {
            if (root) json_decref(root);
            return false;
        }
        SHudView v;
        if (json_t* s = json_object_get(root, "state"); json_is_string(s))
            v.state = stateFromName(json_string_value(s));
        if (json_t* arr = json_object_get(root, "lines"); json_is_array(arr)) {
            size_t  i;
            json_t* o;
            json_array_foreach(arr, i, o) {
                SHudLine ln;
                if (json_t* t = json_object_get(o, "t"); json_is_string(t))
                    ln.text = json_string_value(t);
                if (json_t* c = json_object_get(o, "c"); json_is_string(c))
                    ln.color = colorFromName(json_string_value(c));
                if (json_t* b = json_object_get(o, "big"); json_is_boolean(b))
                    ln.big = json_boolean_value(b);
                v.lines.push_back(std::move(ln));
            }
        }
        auto num = [&](const char* k, double def) -> double {
            json_t* n = json_object_get(root, k);
            if (json_is_number(n)) return json_number_value(n);
            return def;
        };
        auto bl = [&](const char* k, bool def) -> bool {
            json_t* n = json_object_get(root, k);
            if (json_is_boolean(n)) return json_boolean_value(n);
            return def;
        };
        v.confidence   = static_cast<float>(num("conf", -1.0));
        v.approximated = bl("approx", false);
        v.dryRun       = bl("dry", false);
        v.riseMs       = static_cast<int>(num("rise", v.riseMs));
        v.holdMs       = static_cast<int>(num("hold", v.holdMs));
        v.fadeMs       = static_cast<int>(num("fade", v.fadeMs));
        v.opacityCeil  = static_cast<float>(num("ceil", v.opacityCeil));

        json_decref(root);
        out = std::move(v);
        return true;
    }
}
