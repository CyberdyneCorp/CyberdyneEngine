// 2D lights, shadow volumes, the screen-space distance field, and Camera2D. M8.b task 9.5.

#include <cy/rendering/2d/lighting.h>

#include <algorithm>
#include <cmath>

namespace cy::rendering2d {
namespace {

[[nodiscard]] f32 clampf(f32 value, f32 low, f32 high) noexcept {
    return std::clamp(value, low, high);
}

/// The signed area of the triangle (a, b, point). Positive means the point is to the LEFT of the
/// directed edge a→b, which is what decides a one-sided occluder's front face.
[[nodiscard]] f32 side_of(Vec2 a, Vec2 b, Vec2 point) noexcept {
    return ((b.x - a.x) * (point.y - a.y)) - ((b.y - a.y) * (point.x - a.x));
}

/// The distance from a point to a segment, and whether the point is inside a closed polygon, are
/// the two halves of a signed distance. This is the first.
[[nodiscard]] f32 distance_to_segment(Vec2 point, Vec2 a, Vec2 b) noexcept {
    const Vec2 segment = b - a;
    const f32 length_sq = length_squared(segment);
    if (length_sq <= 1e-8F) {
        return length(point - a);
    }
    const f32 t = clampf(dot(point - a, segment) / length_sq, 0.0F, 1.0F);
    return length(point - (a + (segment * t)));
}

/// The crossing-number test: whether a point is inside a closed polygon.
[[nodiscard]] bool inside_polygon(Span<const Vec2> points, Vec2 point) noexcept {
    bool inside = false;
    for (usize index = 0, previous = points.size() - 1; index < points.size(); previous = index++) {
        const Vec2 a = points[index];
        const Vec2 b = points[previous];
        if (((a.y > point.y) != (b.y > point.y)) &&
            (point.x < (((b.x - a.x) * (point.y - a.y)) / (b.y - a.y)) + a.x)) {
            inside = !inside;
        }
    }
    return inside;
}

}  // namespace

Status gather_lights(Span<const Light2D> lights, const Layer2D& layer, Array<u32>& out,
                     LightingReport& report) noexcept {
    out.clear();
    ++report.layers;
    if (!layer.lit) {
        // BEFORE ANY LIGHT IS EXAMINED. "no lighting cost SHALL be incurred for it", and
        // `lights_considered` staying where it was is the measurement.
        ++report.unlit_layers;
        return ok();
    }
    const u32 layer_bit = 1U << (layer.index & 31U);
    for (u32 index = 0; index < static_cast<u32>(lights.size()); ++index) {
        ++report.lights_considered;
        const Light2D& light = lights[index];
        if ((light.layer_mask & layer_bit) == 0U || light.energy <= 0.0F) {
            continue;
        }
        ++report.lights_active;
        if (Status pushed = out.push_back(index); !pushed) {
            return pushed;
        }
    }
    return ok();
}

bool edge_casts(const Light2D& light, Vec2 a, Vec2 b, OccluderCulling culling) noexcept {
    if (culling == OccluderCulling::Both) {
        return true;
    }
    // THE ONE-SIDED CASE. The light is on the edge's front side when it is to the left of a→b for a
    // counter-clockwise polygon, and to the right for a clockwise one — so a light inside a room
    // escapes through the wall's inner face and is blocked from outside.
    const Vec2 reference = (light.kind == Light2DKind::Directional)
                               ? (a - (normalized_or(light.direction, Vec2{0.0F, 1.0F}) * 1e4F))
                               : light.position;
    const f32 side = side_of(a, b, reference);
    return (culling == OccluderCulling::CounterClockwise) ? (side > 0.0F) : (side < 0.0F);
}

Status build_shadow(const Light2D& light, const Occluder2D& occluder, f32 extrusion,
                    Array<ShadowQuad>& out, LightingReport& report) noexcept {
    ++report.occluders_considered;
    if (occluder.points.size() < 2) {
        return ok();
    }
    if (!light.casts_shadows) {
        return ok();
    }

    const usize edges = occluder.closed ? occluder.points.size() : (occluder.points.size() - 1);
    for (usize index = 0; index < edges; ++index) {
        const Vec2 a = occluder.points[index];
        const Vec2 b = occluder.points[(index + 1) % occluder.points.size()];
        if (!edge_casts(light, a, b, occluder.culling)) {
            continue;
        }
        Vec2 away_a;
        Vec2 away_b;
        if (light.kind == Light2DKind::Directional) {
            const Vec2 direction = normalized_or(light.direction, Vec2{0.0F, 1.0F});
            away_a = direction;
            away_b = direction;
        } else {
            away_a = normalized_or(a - light.position, Vec2{1.0F, 0.0F});
            away_b = normalized_or(b - light.position, Vec2{1.0F, 0.0F});
        }
        ShadowQuad quad;
        quad.near_a = a;
        quad.near_b = b;
        quad.far_a = a + (away_a * extrusion);
        quad.far_b = b + (away_b * extrusion);
        if (Status pushed = out.push_back(quad); !pushed) {
            return pushed;
        }
        ++report.shadow_quads;
    }
    return ok();
}

f32 SignedDistanceField::sample(Vec2 world) const noexcept {
    if (width_ == 0 || height_ == 0 || texels_.empty()) {
        return 0.0F;
    }
    const f32 u = (world.x - bounds_.x) / ((bounds_.width > 0.0F) ? bounds_.width : 1.0F);
    const f32 v = (world.y - bounds_.y) / ((bounds_.height > 0.0F) ? bounds_.height : 1.0F);
    const f32 x = clampf(u * static_cast<f32>(width_ - 1), 0.0F, static_cast<f32>(width_ - 1));
    const f32 y = clampf(v * static_cast<f32>(height_ - 1), 0.0F, static_cast<f32>(height_ - 1));
    const auto x0 = static_cast<u32>(x);
    const auto y0 = static_cast<u32>(y);
    const u32 x1 = (x0 + 1U < width_) ? (x0 + 1U) : x0;
    const u32 y1 = (y0 + 1U < height_) ? (y0 + 1U) : y0;
    const f32 fx = x - static_cast<f32>(x0);
    const f32 fy = y - static_cast<f32>(y0);

    const f32 a = texels_[(y0 * width_) + x0];
    const f32 b = texels_[(y0 * width_) + x1];
    const f32 c = texels_[(y1 * width_) + x0];
    const f32 d = texels_[(y1 * width_) + x1];
    const f32 top = a + ((b - a) * fx);
    const f32 bottom = c + ((d - c) * fx);
    return top + ((bottom - top) * fy);
}

Status rasterise_sdf(Span<const Occluder2D> occluders, const Rect2D& viewport,
                     const SdfSettings& settings, SignedDistanceField& out) noexcept {
    const f32 oversize = (settings.oversize > 0.0F) ? settings.oversize : 0.0F;
    Rect2D bounds;
    bounds.x = viewport.x - (viewport.width * oversize * 0.5F);
    bounds.y = viewport.y - (viewport.height * oversize * 0.5F);
    bounds.width = viewport.width * (1.0F + oversize);
    bounds.height = viewport.height * (1.0F + oversize);

    const f32 scale = clampf(settings.scale, 0.05F, 4.0F);
    const auto width = static_cast<u32>(bounds.width * scale);
    const auto height = static_cast<u32>(bounds.height * scale);
    if (width == 0 || height == 0) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "the distance field has no texels", 0});
    }

    out.bounds_ = bounds;
    out.width_ = width;
    out.height_ = height;
    if (Status sized = out.texels_.resize(static_cast<usize>(width) * height); !sized) {
        return sized;
    }

    const f32 limit = (settings.maximum_distance > 0.0F) ? settings.maximum_distance : 64.0F;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const Vec2 point{bounds.x + ((static_cast<f32>(x) + 0.5F) / scale),
                             bounds.y + ((static_cast<f32>(y) + 0.5F) / scale)};
            f32 nearest = limit;
            bool inside = false;
            for (const Occluder2D& occluder : occluders) {
                if (occluder.points.size() < 2) {
                    continue;
                }
                const usize edges =
                    occluder.closed ? occluder.points.size() : (occluder.points.size() - 1);
                for (usize index = 0; index < edges; ++index) {
                    const Vec2 a = occluder.points[index];
                    const Vec2 b = occluder.points[(index + 1) % occluder.points.size()];
                    const f32 distance = distance_to_segment(point, a, b);
                    nearest = (distance < nearest) ? distance : nearest;
                }
                if (occluder.closed && inside_polygon(occluder.points, point)) {
                    inside = true;
                }
            }
            // NEGATIVE INSIDE: the sign is what makes it a SIGNED distance field, and it is what a
            // particle's collision response reads to know which way to push.
            out.texels_[(static_cast<usize>(y) * width) + x] = inside ? -nearest : nearest;
        }
    }
    return ok();
}

void advance_camera(Camera2D& camera, Vec2 target, f32 dt) noexcept {
    if (camera.smoothing_half_life > 0.0F && dt > 0.0F) {
        // HALF-LIFE, not a per-frame factor: the same settle time at 30 and at 144 frames a second.
        const f32 alpha = 1.0F - std::pow(0.5F, dt / camera.smoothing_half_life);
        camera.position = camera.position + ((target - camera.position) * alpha);
    } else {
        camera.position = target;
    }

    if (camera.limited && camera.limits.width > 0.0F && camera.limits.height > 0.0F) {
        camera.position.x =
            clampf(camera.position.x, camera.limits.x, camera.limits.x + camera.limits.width);
        camera.position.y =
            clampf(camera.position.y, camera.limits.y, camera.limits.y + camera.limits.height);
    }

    if (camera.pixel_perfect && camera.pixels_per_unit > 0.0F) {
        // SNAPPED TO A TEXEL. "WHEN pixel-perfect mode is enabled with a nearest filter THEN sprite
        // positions SHALL be snapped so texels map 1:1 to pixels without shimmer" — and snapping
        // the CAMERA rather than each sprite is what keeps their relative positions intact.
        const f32 unit = 1.0F / camera.pixels_per_unit;
        camera.position.x = std::round(camera.position.x / unit) * unit;
        camera.position.y = std::round(camera.position.y / unit) * unit;
    }
}

Rect2D visible_bounds(const Camera2D& camera, Vec2 output) noexcept {
    const f32 zoom = (camera.zoom > 0.0F) ? camera.zoom : 1.0F;
    Vec2 extent{camera.reference.x / zoom, camera.reference.y / zoom};

    switch (camera.strategy) {
        case CanvasStrategy::FixedWithLetterbox:
        case CanvasStrategy::ScaledCanvas:
            // The visible world is the reference canvas, whatever the window is: the difference
            // between the two strategies is what the RENDERER does with the leftover, which is a
            // viewport rectangle rather than a camera decision.
            break;
        case CanvasStrategy::ExpandCanvas: {
            // MORE OF THE WORLD, not a stretched image. The reference height is kept and the width
            // follows the window's aspect.
            const f32 aspect = (output.y > 0.0F) ? (output.x / output.y)
                                                 : (camera.reference.x / camera.reference.y);
            extent.x = (camera.reference.y * aspect) / zoom;
            break;
        }
    }

    const Vec2 centre = camera.position + camera.offset;
    Rect2D bounds;
    bounds.width = extent.x;
    bounds.height = extent.y;
    bounds.x = centre.x - (extent.x * 0.5F);
    bounds.y = centre.y - (extent.y * 0.5F);
    return bounds;
}

Vec2 world_to_view(const Camera2D& camera, Vec2 output, Vec2 world) noexcept {
    const Rect2D bounds = visible_bounds(camera, output);
    Vec2 local{world.x - (bounds.x + (bounds.width * 0.5F)),
               world.y - (bounds.y + (bounds.height * 0.5F))};
    if (camera.rotation != 0.0F) {
        const f32 sine = std::sin(-camera.rotation);
        const f32 cosine = std::cos(-camera.rotation);
        local = Vec2{(local.x * cosine) - (local.y * sine), (local.x * sine) + (local.y * cosine)};
    }
    const f32 scale_x = (bounds.width > 0.0F) ? (output.x / bounds.width) : 1.0F;
    const f32 scale_y = (bounds.height > 0.0F) ? (output.y / bounds.height) : 1.0F;
    return Vec2{(output.x * 0.5F) + (local.x * scale_x), (output.y * 0.5F) + (local.y * scale_y)};
}

}  // namespace cy::rendering2d
