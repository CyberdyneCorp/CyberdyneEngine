// The strategy camera: one zoom parameter, a terrain query, and an edge that scrolls.
// M8.b task 7.3.

#include <cy/camera/strategy.h>

#include <algorithm>
#include <cmath>

namespace cy::camera {
namespace {

[[nodiscard]] f32 clampf(f32 value, f32 low, f32 high) noexcept {
    return std::clamp(value, low, high);
}

[[nodiscard]] f32 mix(f32 a, f32 b, f32 t) noexcept {
    return a + ((b - a) * t);
}

/// Whether a point is inside a convex polygon given in winding order, and the nearest point on it
/// when it is not. One pass, because a map bound is tested every frame for every strategy camera.
[[nodiscard]] Vec2 clamp_to_polygon(Span<const Vec2> polygon, Vec2 point, bool& clamped) noexcept {
    clamped = false;
    if (polygon.size() < 3) {
        return point;
    }
    bool inside = true;
    for (usize index = 0; index < polygon.size(); ++index) {
        const Vec2 a = polygon[index];
        const Vec2 b = polygon[(index + 1) % polygon.size()];
        const Vec2 edge{b.x - a.x, b.y - a.y};
        const Vec2 to_point{point.x - a.x, point.y - a.y};
        if ((edge.x * to_point.y) - (edge.y * to_point.x) < 0.0F) {
            inside = false;
            break;
        }
    }
    if (inside) {
        return point;
    }
    clamped = true;
    Vec2 best = polygon[0];
    f32 best_distance = -1.0F;
    for (usize index = 0; index < polygon.size(); ++index) {
        const Vec2 a = polygon[index];
        const Vec2 b = polygon[(index + 1) % polygon.size()];
        const Vec2 edge{b.x - a.x, b.y - a.y};
        const f32 length_sq = (edge.x * edge.x) + (edge.y * edge.y);
        f32 t = 0.0F;
        if (length_sq > 1e-8F) {
            t = clampf((((point.x - a.x) * edge.x) + ((point.y - a.y) * edge.y)) / length_sq, 0.0F,
                       1.0F);
        }
        const Vec2 candidate{a.x + (edge.x * t), a.y + (edge.y * t)};
        const f32 distance = ((candidate.x - point.x) * (candidate.x - point.x)) +
                             ((candidate.y - point.y) * (candidate.y - point.y));
        if (best_distance < 0.0F || distance < best_distance) {
            best_distance = distance;
            best = candidate;
        }
    }
    return best;
}

}  // namespace

ZoomState evaluate_zoom(const ZoomCurve& curve, f32 normalised_zoom) noexcept {
    // ONE PARAMETER, FOUR CURVES. "so that zooming out raises and tilts the camera coherently
    // rather than through independent controls" — and there is no other function in this module
    // that returns a height.
    const f32 t = clampf(normalised_zoom, 0.0F, 1.0F);
    ZoomState out;
    out.height = mix(curve.near_height, curve.far_height, t);
    out.distance = mix(curve.near_distance, curve.far_distance, t);
    out.tilt_radians = mix(curve.near_tilt, curve.far_tilt, t);
    out.fov_radians = mix(curve.near_fov, curve.far_fov, t);
    return out;
}

Vec2 edge_scroll(const render::ViewportRect& viewport, Vec2 pointer, f32 dead_border) noexcept {
    // DERIVED FROM A POINTER POSITION, which is an action. Nothing here knows what a mouse is, and
    // the viewport is the one the pointer is in — so a split-screen or editor viewport scrolls at
    // its own edges rather than at the window's.
    const f32 width = (viewport.width == 0) ? 1.0F : static_cast<f32>(viewport.width);
    const f32 height = (viewport.height == 0) ? 1.0F : static_cast<f32>(viewport.height);
    const f32 x = (pointer.x - static_cast<f32>(viewport.x)) / width;
    const f32 y = (pointer.y - static_cast<f32>(viewport.y)) / height;
    if (x < 0.0F || x > 1.0F || y < 0.0F || y > 1.0F) {
        // Outside this viewport entirely: the pointer belongs to another player's half, and reading
        // it here is how split-screen edge scrolling gets attributed to the wrong camera.
        return Vec2{0.0F, 0.0F};
    }
    const f32 border = clampf(dead_border, 1e-3F, 0.49F);
    Vec2 pan{0.0F, 0.0F};
    if (x < border) {
        pan.x = -(1.0F - (x / border));
    } else if (x > 1.0F - border) {
        pan.x = 1.0F - ((1.0F - x) / border);
    }
    if (y < border) {
        pan.y = 1.0F - (y / border);
    } else if (y > 1.0F - border) {
        pan.y = -(1.0F - ((1.0F - y) / border));
    }
    pan.x = clampf(pan.x, -1.0F, 1.0F);
    pan.y = clampf(pan.y, -1.0F, 1.0F);
    return pan;
}

Status advance_strategy(StrategyState& state, const StrategyInput& input, const ZoomCurve& curve,
                        const MapBounds& bounds, TerrainHeightService* terrain, Transform& pose,
                        Lens& lens) noexcept {
    if (input.dt < 0.0F) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a strategy camera cannot advance backwards", 0});
    }
    state.zoom = clampf(state.zoom + input.zoom_delta, 0.0F, 1.0F);
    state.yaw += input.yaw_delta;

    // The pan is in the camera's own ground plane, so panning "right" is right on screen at any
    // yaw — which is what a player means by it.
    const f32 sine = std::sin(state.yaw);
    const f32 cosine = std::cos(state.yaw);
    const Vec2 pan{(input.pan.x * cosine) + (input.pan.y * sine),
                   (input.pan.y * cosine) - (input.pan.x * sine)};
    state.anchor.x += pan.x * input.dt;
    state.anchor.z -= pan.y * input.dt;

    // MAP BOUNDS: the polygon wins where one is given, otherwise the rectangle.
    state.at_bounds = false;
    if (bounds.polygon.size() >= 3) {
        bool clamped = false;
        const Vec2 confined =
            clamp_to_polygon(bounds.polygon, Vec2{state.anchor.x, state.anchor.z}, clamped);
        state.anchor.x = confined.x;
        state.anchor.z = confined.y;
        state.at_bounds = clamped;
    } else {
        const f32 x = clampf(state.anchor.x, bounds.region.min.x, bounds.region.max.x);
        const f32 z = clampf(state.anchor.z, bounds.region.min.z, bounds.region.max.z);
        state.at_bounds = (x != state.anchor.x) || (z != state.anchor.z);
        state.anchor.x = x;
        state.anchor.z = z;
    }

    // TERRAIN FOLLOWING: one batched height query, never a cast. A null service leaves the anchor's
    // own height, which is the editor-preview case rather than a special path.
    if (terrain != nullptr) {
        const Vec2 point{state.anchor.x, state.anchor.z};
        f32 height = state.anchor.y;
        terrain->sample_heights(Span<const Vec2>(&point, 1), Span<f32>(&height, 1));
        state.anchor.y = height;
    }

    const ZoomState zoom = evaluate_zoom(curve, state.zoom);
    // The camera sits `distance` back along the yaw and `height` above the anchor, tilted by the
    // curve — three numbers that came out of ONE parameter.
    const Vec3 offset{std::sin(state.yaw) * zoom.distance, zoom.height,
                      std::cos(state.yaw) * zoom.distance};
    pose.translation = state.anchor + offset;
    pose.rotation = Quat::from_euler_yxz(Vec3{-zoom.tilt_radians, state.yaw, 0.0F});
    pose.scale = Vec3{1.0F, 1.0F, 1.0F};

    lens.kind = LensKind::Gameplay;
    lens.gameplay.vertical_fov_radians = zoom.fov_radians;
    return ok();
}

}  // namespace cy::camera
