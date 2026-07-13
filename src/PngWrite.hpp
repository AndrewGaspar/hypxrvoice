#pragma once

#include "HudText.hpp"

#include <string>

// WP-V5 offline evidence path. Writes an SHudImage to a PNG so the HUD is reviewable
// without a headset (the --hud-preview debug mode). Premultiplied source alpha is
// un-premultiplied for a correct on-disk PNG. Uses vendored stb_image_write.

namespace Png {
    // Write `img` to `path`. Returns false on I/O/encoder failure.
    bool write(const SHudImage& img, const std::string& path);
}
