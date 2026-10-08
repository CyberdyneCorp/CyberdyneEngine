#ifndef CY_SERVERS_TEXT_TESTS_FAKE_BACKEND_H
#define CY_SERVERS_TEXT_TESTS_FAKE_BACKEND_H
// A `TextBackend` with no font library behind it, for the server's own logic.
//
// What the server does with a backend — splits text into runs by face, places right-to-left runs in
// visual order, routes a raster to the atlas of its format, preloads a cooked font, counts frames —
// is the server's, and testing it through FreeType would make `unit.text` an integration suite that
// only runs where CY_TEXT is on. This backend is deterministic and trivial instead:
//
//   * a face whose family is "hebrew" has every codepoint from U+0590 up; any other has those
//   below;
//   * the glyph for a codepoint is the codepoint, and every glyph advances ten pixels;
//   * shaping is one glyph per codepoint, reversed for a right-to-left run, as a real shaper's
//     visual order is;
//   * a raster is a 6x8 block of its format: coverage, a distance field for a distance-field face,
//     and colour for U+25A0, the one "colour glyph";
//   * it has no bidirectional algorithm, so the server uses src/text/'s.

#include <cy/servers/text/backend.h>

#include <string_view>

namespace cy::text::test {

class FakeBackend final : public TextBackend {
public:
    static constexpr f32 kAdvance = 10.0f;
    static constexpr u32 kWidth = 6;
    static constexpr u32 kHeight = 8;
    static constexpr Codepoint kColourGlyph = 0x25A0;

    [[nodiscard]] const char* name() const noexcept override { return "fake"; }

    [[nodiscard]] TextCapabilities capabilities() const noexcept override {
        TextCapabilities capabilities;
        capabilities.complex_shaping = true;
        capabilities.bidirectional = true;
        capabilities.colour_glyphs = true;
        capabilities.signed_distance_fields = true;
        capabilities.outline_fonts = true;
        return capabilities;
    }

    [[nodiscard]] Expected<BackendFace, Error> open_face(
        const FontDesc& desc, const FontSource& source) noexcept override {
        if (source.bytes.empty()) {
            return fail(ErrorCode::InvalidArgument, "fake: no bytes");
        }
        if (Status pushed = faces_.push_back(desc); !pushed) {
            return make_unexpected(pushed.error());
        }
        ++open_faces;
        return static_cast<BackendFace>(faces_.size() - 1);
    }

    void close_face(BackendFace /*face*/) noexcept override { --open_faces; }

    [[nodiscard]] FontMetrics face_metrics(BackendFace /*face*/) const noexcept override {
        FontMetrics metrics;
        metrics.ascent = 8.0f;
        metrics.descent = 2.0f;
        metrics.space_advance = kAdvance;
        return metrics;
    }

    [[nodiscard]] GlyphIndex glyph_for(BackendFace face,
                                       Codepoint codepoint) const noexcept override {
        const bool hebrew = faces_[face].family == "hebrew";
        return (codepoint >= 0x0590) == hebrew ? static_cast<GlyphIndex>(codepoint) : kNotdef;
    }

    [[nodiscard]] Status rasterise(BackendFace face, GlyphIndex glyph,
                                   GlyphRaster& out) noexcept override {
        ++rasterisations;
        out.format = glyph == kColourGlyph ? PixelFormat::Colour
                     : faces_[face].mode == RenderMode::SignedDistanceField
                         ? PixelFormat::DistanceField
                         : PixelFormat::Coverage;
        out.metrics.advance = kAdvance;
        out.metrics.bearing_x = 1.0f;
        out.metrics.bearing_y = -8.0f;
        out.metrics.width = kWidth;
        out.metrics.height = kHeight;
        if (Status resized = out.pixels.resize(kWidth * kHeight * bytes_per_pixel(out.format));
            !resized) {
            return resized;
        }
        for (u8& byte : out.pixels) {
            byte = static_cast<u8>(glyph & 0xFFU);
        }
        return ok();
    }

    [[nodiscard]] Status shape(BackendFace /*face*/, const ShapeRequest& request,
                               Array<BackendGlyph>& out) noexcept override {
        out.clear();
        usize cursor = 0;
        while (cursor < request.text.size()) {
            BackendGlyph glyph;
            glyph.cluster = static_cast<u32>(cursor);
            glyph.glyph = decode_utf8(request.text, cursor);
            glyph.advance = Vec2{kAdvance, 0.0f};
            if (Status pushed = out.push_back(glyph); !pushed) {
                return pushed;
            }
        }
        if (request.direction == Direction::RightToLeft) {
            for (usize low = 0, high = out.size(); low + 1 < high; ++low, --high) {
                const BackendGlyph held = out[low];
                out[low] = out[high - 1];
                out[high - 1] = held;
            }
        }
        ++shapes;
        return ok();
    }

    u32 rasterisations = 0;
    u32 shapes = 0;
    i32 open_faces = 0;

private:
    Array<FontDesc> faces_;
};

/// Any non-empty bytes: the fake backend never reads them.
inline constexpr u8 kFakeFontBytes[] = {1, 2, 3, 4};

inline FontSource fake_source() {
    return FontSource{Span<const u8>(kFakeFontBytes, sizeof(kFakeFontBytes)), 0};
}

inline FontDesc fake_desc(std::string_view family, RenderMode mode = RenderMode::Grayscale) {
    FontDesc desc;
    desc.family = family;
    desc.size_pixels = 10.0f;
    desc.mode = mode;
    return desc;
}

}  // namespace cy::text::test

#endif  // CY_SERVERS_TEXT_TESTS_FAKE_BACKEND_H
