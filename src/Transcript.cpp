#include "Transcript.hpp"

#include <string>

namespace {
    // Minimal JSON string escaper — enough for transcript text and phrases.
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
}

std::string STranscript::toJson() const {
    std::string o;
    o.reserve(128 + text.size());
    o += '{';
    o += "\"text\":";
    appendEscaped(o, text);
    o += ",\"onsetMs\":" + std::to_string(onsetMs);
    o += ",\"endMs\":" + std::to_string(endMs);
    o += ",\"wallMs\":" + std::to_string(wallMs);
    o += ",\"activation\":\"";
    o += activationName(activation);
    o += '"';
    if (!wakePhrase.empty()) {
        o += ",\"wakePhrase\":";
        appendEscaped(o, wakePhrase);
    }
    o += ",\"words\":[";
    for (size_t i = 0; i < words.size(); i++) {
        if (i)
            o += ',';
        o += '{';
        o += "\"text\":";
        appendEscaped(o, words[i].text);
        o += ",\"startMs\":" + std::to_string(words[i].startMs);
        o += ",\"endMs\":" + std::to_string(words[i].endMs);
        if (words[i].prob >= 0.f) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.3f", words[i].prob);
            o += ",\"prob\":";
            o += buf;
        }
        o += '}';
    }
    o += "]}";
    return o;
}
