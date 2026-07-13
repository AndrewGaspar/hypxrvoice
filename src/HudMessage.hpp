#pragma once

#include "HudModel.hpp"

#include <string>

// WP-V5 HUD IPC wire format. The daemon computes SHudView (pure model) and streams
// it as one compact JSON line per update to the `hypxrvoice-hud` subprocess's stdin;
// the subprocess parses it back and rasterises + submits. Keeping the model on the
// daemon side means ALL feedback logic (phrasing, layout selection, fade envelope)
// stays in the testable core and the subprocess is a thin renderer. Round-trips
// exactly, so it is unit-tested directly.

namespace HudMsg {
    // Serialise a view to a single line (no embedded newline; '\n'-terminated).
    std::string serialize(const SHudView& v);

    // Parse a line back into a view. Returns false on malformed input (the subprocess
    // then ignores the line rather than dying).
    bool parse(const std::string& line, SHudView& out);
}
