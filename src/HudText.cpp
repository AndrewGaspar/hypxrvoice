#include "HudText.hpp"

#include "stb_truetype.h" // implementation in stb_impl.cpp

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

// The bundled OFL font, baked into the binary by CMake (font_data.c). See
// third_party/fonts/LiberationMono-OFL.txt for the licence.
extern "C" const unsigned char g_hudFontData[];
extern "C" const unsigned int  g_hudFontDataSize;

namespace {
    struct SRgb { uint8_t r, g, b; };

    SRgb colorFor(EHudColor c) {
        switch (c) {
            case EHudColor::Normal: return {235, 238, 242};
            case EHudColor::Dim:    return {150, 158, 168};
            case EHudColor::Accent: return {120, 190, 255};
            case EHudColor::Good:   return {120, 220, 150};
            case EHudColor::Warn:   return {250, 200, 110};
            case EHudColor::Bad:    return {255, 120, 120};
        }
        return {235, 238, 242};
    }

    // stb font, initialised once.
    struct SFont {
        stbtt_fontinfo info{};
        bool           ok = false;
        SFont() {
            ok = stbtt_InitFont(&info, g_hudFontData,
                                stbtt_GetFontOffsetForIndex(g_hudFontData, 0)) != 0;
        }
    };
    const SFont& font() {
        static SFont f;
        return f;
    }

    // Minimal UTF-8 decode -> codepoint. Advances `i`. Returns U+FFFD on bad bytes.
    uint32_t nextCodepoint(const std::string& s, size_t& i) {
        auto b0 = static_cast<unsigned char>(s[i]);
        if (b0 < 0x80) { i += 1; return b0; }
        auto cont = [&](size_t k) {
            return k < s.size() && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80;
        };
        if ((b0 & 0xE0) == 0xC0 && cont(i + 1)) {
            uint32_t cp = ((b0 & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
            i += 2; return cp;
        }
        if ((b0 & 0xF0) == 0xE0 && cont(i + 1) && cont(i + 2)) {
            uint32_t cp = ((b0 & 0x0F) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                          (static_cast<unsigned char>(s[i + 2]) & 0x3F);
            i += 3; return cp;
        }
        if ((b0 & 0xF8) == 0xF0 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
            uint32_t cp = ((b0 & 0x07) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
                          ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
                          (static_cast<unsigned char>(s[i + 3]) & 0x3F);
            i += 4; return cp;
        }
        i += 1;
        return 0xFFFD;
    }

    // A premultiplied-alpha RGBA canvas with simple over-compositing helpers.
    struct SCanvas {
        int                  w, h;
        std::vector<uint8_t> px; // premultiplied, top-row-first.
        SCanvas(int W, int H) : w(W), h(H), px(static_cast<size_t>(W) * H * 4, 0) {}

        // Composite a premultiplied source pixel (sr,sg,sb already * sa) over dst.
        void blend(int x, int y, float sr, float sg, float sb, float sa) {
            if (x < 0 || y < 0 || x >= w || y >= h || sa <= 0.f)
                return;
            uint8_t* d   = &px[(static_cast<size_t>(y) * w + x) * 4];
            float    ida = 1.f - sa;
            d[0] = static_cast<uint8_t>(std::min(255.f, sr * 255.f + d[0] * ida));
            d[1] = static_cast<uint8_t>(std::min(255.f, sg * 255.f + d[1] * ida));
            d[2] = static_cast<uint8_t>(std::min(255.f, sb * 255.f + d[2] * ida));
            d[3] = static_cast<uint8_t>(std::min(255.f, sa * 255.f + d[3] * ida));
        }
    };

    // Rounded-rectangle fill (premultiplied). color is straight RGB in [0,1], a is alpha.
    void fillRoundRect(SCanvas& c, int x0, int y0, int x1, int y1, int rad, float r, float g,
                       float b, float a) {
        rad = std::max(0, std::min(rad, std::min((x1 - x0) / 2, (y1 - y0) / 2)));
        for (int y = std::max(0, y0); y < std::min(c.h, y1); y++) {
            for (int x = std::max(0, x0); x < std::min(c.w, x1); x++) {
                float cov = 1.f;
                // corner distance test for rounded edges (1px AA).
                int cx = -1, cy = -1;
                if (x < x0 + rad && y < y0 + rad) { cx = x0 + rad; cy = y0 + rad; }
                else if (x >= x1 - rad && y < y0 + rad) { cx = x1 - rad - 1; cy = y0 + rad; }
                else if (x < x0 + rad && y >= y1 - rad) { cx = x0 + rad; cy = y1 - rad - 1; }
                else if (x >= x1 - rad && y >= y1 - rad) { cx = x1 - rad - 1; cy = y1 - rad - 1; }
                if (cx >= 0) {
                    float d = std::sqrt(static_cast<float>((x - cx) * (x - cx) + (y - cy) * (y - cy)));
                    cov = std::clamp(rad - d + 0.5f, 0.f, 1.f);
                }
                float sa = a * cov;
                c.blend(x, y, r * sa, g * sa, b * sa, sa);
            }
        }
    }

    struct SLineMetrics {
        float scale;
        int   ascent, descent, lineGap;
        int   width; // pixel advance width of the whole string.
    };

    SLineMetrics measure(const std::string& text, float pixelHeight) {
        SLineMetrics m{};
        const auto&  f = font();
        m.scale        = stbtt_ScaleForPixelHeight(&f.info, pixelHeight);
        int a, d, g;
        stbtt_GetFontVMetrics(&f.info, &a, &d, &g);
        m.ascent  = static_cast<int>(std::round(a * m.scale));
        m.descent = static_cast<int>(std::round(d * m.scale));
        m.lineGap = static_cast<int>(std::round(g * m.scale));
        float xadv = 0.f;
        for (size_t i = 0; i < text.size();) {
            uint32_t cp = nextCodepoint(text, i);
            int      aw, lsb;
            stbtt_GetCodepointHMetrics(&f.info, static_cast<int>(cp), &aw, &lsb);
            xadv += aw * m.scale;
        }
        m.width = static_cast<int>(std::ceil(xadv));
        return m;
    }

    // Draw a string at baseline (penX,penY) in `color`. Returns advance width.
    void drawText(SCanvas& c, const std::string& text, int penX, int penY, float pixelHeight,
                  SRgb color) {
        const auto& f = font();
        float       scale = stbtt_ScaleForPixelHeight(&f.info, pixelHeight);
        float       x     = static_cast<float>(penX);
        const float cr = color.r / 255.f, cg = color.g / 255.f, cb = color.b / 255.f;
        for (size_t i = 0; i < text.size();) {
            uint32_t cp = nextCodepoint(text, i);
            int      aw, lsb;
            stbtt_GetCodepointHMetrics(&f.info, static_cast<int>(cp), &aw, &lsb);

            int gw = 0, gh = 0, xoff = 0, yoff = 0;
            unsigned char* bmp = stbtt_GetCodepointBitmap(&f.info, 0, scale, static_cast<int>(cp),
                                                          &gw, &gh, &xoff, &yoff);
            if (bmp) {
                int gx0 = static_cast<int>(std::round(x)) + xoff;
                int gy0 = penY + yoff;
                for (int gy = 0; gy < gh; gy++)
                    for (int gx = 0; gx < gw; gx++) {
                        float cov = bmp[gy * gw + gx] / 255.f;
                        if (cov <= 0.f)
                            continue;
                        c.blend(gx0 + gx, gy0 + gy, cr * cov, cg * cov, cb * cov, cov);
                    }
                stbtt_FreeBitmap(bmp, nullptr);
            }
            x += aw * scale;
        }
    }
}

SHudImage renderHud(const SHudView& v, int texW, int texH) {
    SHudImage img;
    img.w = texW;
    img.h = texH;
    img.rgba.assign(static_cast<size_t>(texW) * texH * 4, 0);

    if (v.state == EHudState::Hidden || v.lines.empty() || !font().ok)
        return img; // fully transparent.

    SCanvas c(texW, texH);

    const float titlePx = std::round(texH * 0.135f); // ~52 px at 384.
    const float bodyPx  = std::round(texH * 0.095f); // ~36 px at 384.
    const int   padX    = static_cast<int>(std::round(texW * 0.045f));
    const int   padY    = static_cast<int>(std::round(texH * 0.06f));
    const int   lineGap = static_cast<int>(std::round(bodyPx * 0.35f));

    // Measure lines to size the panel.
    struct SLaid {
        const SHudLine* line;
        float           px;
        SLineMetrics    m;
    };
    std::vector<SLaid> laid;
    int contentH = 0, contentW = 0;
    for (const auto& ln : v.lines) {
        float px = ln.big ? titlePx : bodyPx;
        SLineMetrics m = measure(ln.text, px);
        laid.push_back({&ln, px, m});
        contentH += (m.ascent - m.descent);
        contentW = std::max(contentW, m.width);
        contentH += lineGap;
    }
    // Confidence bar occupies one extra body row.
    const bool showBar = v.confidence >= 0.f;
    const int  barH    = static_cast<int>(std::round(bodyPx * 0.28f));
    if (showBar)
        contentH += barH + lineGap;
    if (!laid.empty())
        contentH -= lineGap; // no trailing gap.

    int panelW = std::min(texW - 8, contentW + 2 * padX);
    int panelH = std::min(texH - 8, contentH + 2 * padY);
    int panelX0 = (texW - panelW) / 2;
    int panelY0 = (texH - panelH) / 2;

    // Panel background: translucent dark with a subtle accent edge tint on error.
    float bgA = 0.66f;
    fillRoundRect(c, panelX0, panelY0, panelX0 + panelW, panelY0 + panelH,
                  static_cast<int>(std::round(texH * 0.05f)), 0.05f, 0.06f, 0.08f, bgA);

    // Lay out lines from the top of the content region.
    int penY = panelY0 + padY;
    for (auto& l : laid) {
        penY += l.m.ascent;
        int textX = panelX0 + padX;
        drawText(c, l.line->text, textX, penY, l.px, colorFor(l.line->color));
        penY += (-l.m.descent) + lineGap;

        // Draw the confidence bar right after the title line (first line).
        if (showBar && l.line == &v.lines.front()) {
            int barX0 = panelX0 + padX;
            int barX1 = panelX0 + panelW - padX;
            int barY0 = penY;
            int barY1 = penY + barH;
            // track
            fillRoundRect(c, barX0, barY0, barX1, barY1, barH / 2, 0.25f, 0.27f, 0.3f, 0.55f);
            // fill
            int fillW = static_cast<int>(std::round((barX1 - barX0) * std::clamp(v.confidence, 0.f, 1.f)));
            SRgb bc2 = v.confidence >= 0.75f ? colorFor(EHudColor::Good)
                       : v.confidence >= 0.5f ? colorFor(EHudColor::Warn)
                                              : colorFor(EHudColor::Bad);
            fillRoundRect(c, barX0, barY0, barX0 + std::max(barH, fillW), barY1, barH / 2,
                          bc2.r / 255.f, bc2.g / 255.f, bc2.b / 255.f, 0.95f);
            penY += barH + lineGap;
        }
    }

    img.rgba = std::move(c.px);
    return img;
}
