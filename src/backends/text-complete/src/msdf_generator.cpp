// SPDX-License-Identifier: MIT
// msdfgen: the one translation unit that names it. See internal.h for what crosses out of here.
#include "internal.h"

#include <msdfgen.h>

#include <algorithm>
#include <cmath>

namespace cy::text::detail {
namespace {

/// The angle, in radians, below which a corner gets differently coloured edges. msdfgen's own
/// recommended default: sharper corners than about 171 degrees stay sharp at any scale.
constexpr double kCornerAngle = 3.0;

[[nodiscard]] msdfgen::Point2 point_of(const GlyphOutline::Point& point) noexcept {
    return msdfgen::Point2(point.x, point.y);
}

/// The engine's outline as msdfgen's shape: one contour per Move, the segments kept as the curves
/// they are. A segment that does not move the pen is dropped, as msdfgen's own FreeType importer
/// drops it, because a zero-length edge has no direction to colour.
void build_shape(const GlyphOutline& outline, msdfgen::Shape& shape) noexcept {
    msdfgen::Contour* contour = nullptr;
    msdfgen::Point2 pen;
    usize next = 0;
    for (const GlyphOutline::Verb verb : outline.verbs) {
        const usize used = verb == GlyphOutline::Verb::Cubic       ? 3
                           : verb == GlyphOutline::Verb::Quadratic ? 2
                                                                   : 1;
        const usize first = next;
        next += used;
        const msdfgen::Point2 end = point_of(outline.points[next - 1]);
        if (verb == GlyphOutline::Verb::Move) {
            if (contour == nullptr || !contour->edges.empty()) {
                contour = &shape.addContour();
            }
            pen = end;
            continue;
        }
        if (contour == nullptr || end == pen) {
            continue;
        }
        if (verb == GlyphOutline::Verb::Line) {
            contour->addEdge(msdfgen::EdgeHolder(pen, end));
        } else if (verb == GlyphOutline::Verb::Quadratic) {
            contour->addEdge(msdfgen::EdgeHolder(pen, point_of(outline.points[first]), end));
        } else {
            contour->addEdge(msdfgen::EdgeHolder(pen, point_of(outline.points[first]),
                                                 point_of(outline.points[first + 1]), end));
        }
        pen = end;
    }
    if (!shape.contours.empty() && shape.contours.back().edges.empty()) {
        shape.contours.pop_back();
    }
}

[[nodiscard]] u8 to_byte(float value) noexcept {
    return static_cast<u8>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

}  // namespace

Status msdf_generate(const GlyphOutline& outline, f32 range, GlyphRaster& out) noexcept {
    out.format = PixelFormat::DistanceField;
    out.metrics = GlyphMetrics{};
    out.metrics.advance = outline.advance;
    out.pixels.clear();
    if (!(range > 0.0f)) {
        return fail(ErrorCode::InvalidArgument, "a distance field needs a positive range");
    }

    msdfgen::Shape shape;
    build_shape(outline, shape);
    if (shape.contours.empty()) {
        // A space, or any glyph with no ink: nothing to draw, and the advance is all it has.
        return ok();
    }
    shape.normalize();
    msdfgen::edgeColoringSimple(shape, kCornerAngle);

    // The bitmap is the outline's bounds on whole pixels, grown by the range on every side so the
    // field has room to fall to zero outside the glyph — which is what lets an outline effect of up
    // to `range` pixels be drawn from it.
    const msdfgen::Shape::Bounds bounds = shape.getBounds();
    const double pad = std::ceil(static_cast<double>(range));
    const double left = std::floor(bounds.l) - pad;
    const double bottom = std::floor(bounds.b) - pad;
    const auto width = static_cast<int>(std::ceil(bounds.r) + pad - left);
    const auto height = static_cast<int>(std::ceil(bounds.t) + pad - bottom);

    msdfgen::Bitmap<float, 4> field(width, height);
    const msdfgen::SDFTransformation transformation(
        msdfgen::Projection(msdfgen::Vector2(1.0, 1.0), msdfgen::Vector2(-left, -bottom)),
        msdfgen::DistanceMapping(msdfgen::Range(2.0 * static_cast<double>(range))));
    msdfgen::generateMTSDF(field, shape, transformation, msdfgen::MSDFGeneratorConfig(true));

    // TrueType and CFF outlines wind in opposite directions, and msdfgen's sign follows the
    // winding. The bottom-left pixel is `range` pixels outside the outline by construction, so if
    // it reads as inside the whole field is inverted, and inverting it is exact.
    const bool inverted = field(0, 0)[3] > 0.5f;

    if (Status resized = out.pixels.resize(static_cast<usize>(width) * height * 4U); !resized) {
        return resized;
    }
    for (int y = 0; y < height; ++y) {
        // msdfgen's row 0 is the bottom; the atlas's is the top.
        const int source_row = height - 1 - y;
        u8* target = out.pixels.data() + (static_cast<usize>(y) * static_cast<usize>(width) * 4U);
        for (int x = 0; x < width; ++x) {
            const float* texel = field(x, source_row);
            for (int channel = 0; channel < 4; ++channel) {
                const float value = inverted ? 1.0f - texel[channel] : texel[channel];
                target[(x * 4) + channel] = to_byte(value);
            }
        }
    }
    out.metrics.bearing_x = static_cast<f32>(left);
    out.metrics.bearing_y = -static_cast<f32>(bottom + height);
    out.metrics.width = static_cast<u32>(width);
    out.metrics.height = static_cast<u32>(height);
    return ok();
}

}  // namespace cy::text::detail
