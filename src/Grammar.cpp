#include "Grammar.hpp"

namespace {
    // Escape a monitor name for a GBNF double-quoted literal. Names are compositor
    // output identifiers (e.g. "XR-code"); still, quote/backslash are escaped.
    std::string gbnfLiteral(const std::string& s) {
        std::string o = "\"\\\""; // opening \"
        for (char c : s) {
            if (c == '"' || c == '\\')
                o += '\\';
            o += c;
        }
        o += "\\\"\""; // closing \"
        return o;
    }
}

namespace Grammar {
    std::string buildIntentGrammar(const std::vector<std::string>& monitorNames) {
        std::string g;
        g += "root ::= \"{\" ws ";
        g += "\"\\\"verb\\\":\" ws verb \",\" ws ";
        g += "\"\\\"target\\\":\" ws target \",\" ws ";
        g += "\"\\\"deictic\\\":\" ws bool \",\" ws ";
        g += "\"\\\"place\\\":\" ws bool \",\" ws ";
        g += "\"\\\"anchor\\\":\" ws anchor \",\" ws ";
        g += "\"\\\"sub\\\":\" ws sub \",\" ws ";
        g += "\"\\\"deltaM\\\":\" ws number \",\" ws ";
        g += "\"\\\"workspace\\\":\" ws number \",\" ws ";
        g += "\"\\\"app\\\":\" ws str \",\" ws ";
        g += "\"\\\"confidence\\\":\" ws number ws \"}\"\n";

        g += "verb ::= \"\\\"none\\\"\" | \"\\\"clarify\\\"\" | \"\\\"pick\\\"\" | "
             "\"\\\"place\\\"\" | \"\\\"move_dist\\\"\" | \"\\\"center\\\"\" | "
             "\"\\\"dock\\\"\" | \"\\\"undock\\\"\" | \"\\\"follow\\\"\" | "
             "\"\\\"anchor\\\"\" | \"\\\"hand_input\\\"\" | \"\\\"launch_app\\\"\" | "
             "\"\\\"focus\\\"\" | \"\\\"fullscreen\\\"\" | \"\\\"workspace\\\"\" | "
             "\"\\\"move_window\\\"\"\n";

        // target: one of the live monitor names, else "active" or "".
        g += "target ::= \"\\\"active\\\"\" | \"\\\"\\\"\"";
        for (auto& n : monitorNames)
            g += " | " + gbnfLiteral(n);
        g += "\n";

        g += "anchor ::= \"\\\"\\\"\" | \"\\\"local\\\"\" | \"\\\"head\\\"\" | "
             "\"\\\"body\\\"\" | \"\\\"device:left\\\"\" | \"\\\"device:right\\\"\"\n";
        g += "sub ::= \"\\\"\\\"\" | \"\\\"on\\\"\" | \"\\\"off\\\"\" | "
             "\"\\\"toggle\\\"\" | \"\\\"auto\\\"\" | \"\\\"here\\\"\" | "
             "\"\\\"head\\\"\" | \"\\\"body\\\"\"\n";
        g += "bool ::= \"true\" | \"false\"\n";
        g += "number ::= \"-\"? [0-9]+ (\".\" [0-9]+)?\n";
        // A short free-text string for the spoken app name; bounded charset only.
        g += "str ::= \"\\\"\" [a-zA-Z0-9 ._-]* \"\\\"\"\n";
        g += "ws ::= [ \\t\\n]*\n";
        return g;
    }
}
