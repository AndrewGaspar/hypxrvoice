#pragma once

#include "HudModel.hpp"

#include <cstdint>
#include <vector>

// WP-V5 HUD text rasteriser. Turns an SHudView (pure model) into an RGBA image with
// PREMULTIPLIED alpha, top-row-first, ready either to be written as a PNG
// (--hud-preview) or uploaded verbatim into the overlay swapchain. Uses the vendored
// stb_truetype with a bundled OFL font (third_party/fonts/LiberationMono) baked into
// the binary — no runtime font-path dependency, no system font libs. CPU only; no
// OpenXR/EGL/GL here, so it lives in the shared hud_core and is exercised offline.

struct SHudImage {
    int                  w = 0;
    int                  h = 0;
    std::vector<uint8_t> rgba; // w*h*4, premultiplied, top row first.
    bool                 empty() const { return rgba.empty(); }
};

// Render the view into a texW x texH transparent canvas with a compact rounded panel
// hugging the content (so transparent margins let the world/monitors show through).
// A Hidden view yields a fully transparent image. Deterministic — same view in, same
// pixels out (used as the offline correctness surface).
SHudImage renderHud(const SHudView& v, int texW = 768, int texH = 384);
