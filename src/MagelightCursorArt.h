#pragma once

// Magelight - the flat cursor's art, drawn in code (no PNG): a faceted steel arrowhead with an ink outline and a soft
// shadow, a brass glow while the page shows a clickable cursor, and an I-beam over text. Rasterized from signed
// distances at the exact pixel height it is drawn at, so it stays crisp at every resolution. A Palette recolours it
// (SetViewCursorTint, 0.31.1). Engine-free: it returns premultiplied BGRA pixels, the encoding every composited surface
// uses.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Magelight::CursorArt
{
    struct Image
    {
        int w = 0, h = 0;
        float hotX = 0.0f, hotY = 0.0f;   // the pointer pixel, in pixels from the top-left
        std::vector<std::uint32_t> px;    // premultiplied BGRA, row-major
    };

    struct Rgb { float r, g, b; };

    /// The five colours the art is drawn in. The default is the host's own look.
    struct Palette
    {
        Rgb lit{ 0xd8 / 255.0f, 0xcc / 255.0f, 0xb0 / 255.0f };    // the arrow's lit facet (steel)
        Rgb shade{ 0x7d / 255.0f, 0x6e / 255.0f, 0x52 / 255.0f };  // the arrow's shaded facet
        Rgb ink{ 0x12 / 255.0f, 0x0e / 255.0f, 0x0a / 255.0f };    // the outline
        Rgb glow{ 0xec / 255.0f, 0xc9 / 255.0f, 0x83 / 255.0f };   // the hover glow (brass)
        Rgb ibeam{ 0xf0 / 255.0f, 0xe4 / 255.0f, 0xc9 / 255.0f };  // the I-beam (vellum)
    };

    namespace detail
    {
        struct P { float x, y; };

        // Units of the cursor height: the tip (the hotspot) at 0,0, the body to the lower right.
        inline constexpr std::array<P, 7> kArrow{ { { 0.0f, 0.0f }, { 0.0f, 0.74f }, { 0.18f, 0.60f }, { 0.30f, 0.88f },
                                                   { 0.41f, 0.83f }, { 0.29f, 0.56f }, { 0.52f, 0.56f } } };
        inline constexpr P kFacetAxis{ 0.30f, 0.72f };   // tip -> tail: the lit facet is left of it
        inline constexpr float kArrowW = 0.52f, kArrowH = 0.88f;

        using Magelight::CursorArt::Rgb;

        // Signed distance to a closed polygon: negative inside.
        template <std::size_t N>
        float SdPolygon(float x, float y, const std::array<P, N>& pts, float scale)
        {
            float d = 1e9f;
            bool inside = false;
            for (std::size_t i = 0, j = N - 1; i < N; j = i++) {
                const float ax = pts[j].x * scale, ay = pts[j].y * scale;
                const float bx = pts[i].x * scale, by = pts[i].y * scale;
                const float ex = bx - ax, ey = by - ay;
                const float t = std::clamp(((x - ax) * ex + (y - ay) * ey) / (ex * ex + ey * ey), 0.0f, 1.0f);
                d = std::min(d, std::hypot(x - (ax + t * ex), y - (ay + t * ey)));
                if (((ay > y) != (by > y)) && (x < (bx - ax) * (y - ay) / (by - ay) + ax)) inside = !inside;
            }
            return inside ? -d : d;
        }

        inline float SdCapsule(float x, float y, P a, P b, float r, float scale)
        {
            const float ax = a.x * scale, ay = a.y * scale, ex = (b.x - a.x) * scale, ey = (b.y - a.y) * scale;
            const float t = std::clamp(((x - ax) * ex + (y - ay) * ey) / (ex * ex + ey * ey), 0.0f, 1.0f);
            return std::hypot(x - (ax + t * ex), y - (ay + t * ey)) - r * scale;
        }

        // Antialiased coverage of "signed distance below e".
        inline float Cov(float sd, float e) { return std::clamp(e - sd + 0.5f, 0.0f, 1.0f); }

        struct Px
        {
            float r = 0, g = 0, b = 0, a = 0;   // premultiplied
            void Over(Rgb c, float alpha)
            {
                alpha = std::clamp(alpha, 0.0f, 1.0f);
                r = c.r * alpha + r * (1 - alpha);
                g = c.g * alpha + g * (1 - alpha);
                b = c.b * alpha + b * (1 - alpha);
                a = alpha + a * (1 - alpha);
            }
            std::uint32_t Bgra() const
            {
                const auto q = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
                return (q(a) << 24) | (q(r) << 16) | (q(g) << 8) | q(b);
            }
        };

        inline float OutlineWidth(int height) { return std::max(1.0f, 0.045f * static_cast<float>(height)); }
        // Room for the glow; whole pixels, so the tip vertex (the hotspot) sits on a pixel corner.
        inline float Pad(int height) { return std::ceil(OutlineWidth(height) + 0.28f * static_cast<float>(height)); }

        // The glow band, the drop shadow, the ink outline: shared by every shape, from the union distance.
        inline void Under(Px& p, float sd, int height, float glow, const Palette& pal)
        {
            const float h = static_cast<float>(height);
            if (glow > 0.0f) {
                const float g = std::clamp(1.0f - std::max(sd, 0.0f) / (0.24f * h), 0.0f, 1.0f);
                p.Over(pal.glow, g * g * glow * 0.85f);
            }
            const float shadow = std::clamp(1.0f - std::max(sd - 0.5f, 0.0f) / (0.07f * h + 1.2f), 0.0f, 1.0f);
            p.Over(Rgb{ 0, 0, 0 }, shadow * 0.55f);
            p.Over(pal.ink, Cov(sd, 0.0f));
        }
    }

    /// The arrowhead at `height` pixels; glow 0..1 is the hover glow's strength. Every glow level has the same size and
    /// hotspot, so a fade between them only swaps the texture.
    inline Image Arrow(int height, float glow, const Palette& pal = Palette{})
    {
        using namespace detail;
        height = std::max(height, 8);
        const float h = static_cast<float>(height), pad = Pad(height), ow = OutlineWidth(height);
        Image img;
        img.w = static_cast<int>(std::ceil(kArrowW * h + 2 * pad));
        img.h = static_cast<int>(std::ceil(kArrowH * h + 2 * pad));
        img.hotX = img.hotY = pad;
        img.px.resize(static_cast<std::size_t>(img.w) * img.h);
        for (int y = 0; y < img.h; ++y) {
            for (int x = 0; x < img.w; ++x) {
                const float sx = x + 0.5f - pad, sy = y + 0.5f - pad;
                const float sd = SdPolygon(sx, sy, kArrow, h);
                Px p;
                Under(p, sd, height, glow, pal);
                const bool lit = (sx * kFacetAxis.y - sy * kFacetAxis.x) < 0.0f;
                p.Over(lit ? pal.lit : pal.shade, Cov(sd, -ow));
                img.px[static_cast<std::size_t>(y) * img.w + x] = p.Bgra();
            }
        }
        return img;
    }

    /// The I-beam shown over text, `height` pixels tall; the hotspot is its centre.
    inline Image IBeam(int height, const Palette& pal = Palette{})
    {
        using namespace detail;
        height = std::max(height, 8);
        const float h = static_cast<float>(height), pad = Pad(height), ow = OutlineWidth(height);
        Image img;
        img.w = static_cast<int>(std::ceil(0.60f * h + 2 * pad));
        img.h = static_cast<int>(std::ceil(1.00f * h + 2 * pad));
        img.hotX = pad + 0.30f * h;
        img.hotY = pad + 0.50f * h;
        img.px.resize(static_cast<std::size_t>(img.w) * img.h);
        for (int y = 0; y < img.h; ++y) {
            for (int x = 0; x < img.w; ++x) {
                const float sx = x + 0.5f - pad, sy = y + 0.5f - pad;
                const float sd = std::min({ SdCapsule(sx, sy, { 0.30f, 0.12f }, { 0.30f, 0.88f }, 0.06f, h),
                                            SdCapsule(sx, sy, { 0.18f, 0.10f }, { 0.42f, 0.10f }, 0.06f, h),
                                            SdCapsule(sx, sy, { 0.18f, 0.90f }, { 0.42f, 0.90f }, 0.06f, h) });
                Px p;
                Under(p, sd, height, 0.0f, pal);
                p.Over(pal.ibeam, Cov(sd, -ow));
                img.px[static_cast<std::size_t>(y) * img.w + x] = p.Bgra();
            }
        }
        return img;
    }
}
