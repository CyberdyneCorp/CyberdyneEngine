// SPDX-License-Identifier: MIT
// The navigation overlay, rasterised onto the frame canvas. See nav_overlay.h.

#include "nav_overlay.h"

#include <cy/servers/render/picking.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cy::sample::editor_window {
namespace {

namespace nav = navigation;

constexpr u32 kGround = 0x0028'B4E6U;
constexpr u32 kCarved = 0x00E0'3C3CU;
constexpr u32 kTileBorder = 0x00F0'F0F0U;
constexpr u32 kLink = 0x00F2'B33DU;
constexpr u32 kObstacle = 0x00F0'5A28U;
constexpr u32 kPath = 0x00FF'E650U;
constexpr u32 kFlowReachable = 0x0078'F08CU;
constexpr u32 kFlowBlocked = 0x0090'9090U;
/// Every other area's colour, by `area % 8`.
constexpr u32 kAreaPalette[8] = {0x0028'B4E6U, 0x00E6'A028U, 0x0096'E650U, 0x00B4'64F0U,
                                 0x00F0'DC46U, 0x0046'DCC8U, 0x00F0'78B4U, 0x008C'8CF0U};

constexpr f32 kPolygonAlpha = 0.45F;
constexpr f32 kLineAlpha = 0.9F;
/// The longest line walked, in pixels. A segment is clipped to the canvas first, so this only
/// bounds a pathological projection.
constexpr f32 kMaxLineSteps = 8192.0F;

void blend(const Canvas& canvas, i32 x, i32 y, u32 colour, f32 alpha) noexcept {
    if (x < 0 || y < 0 || std::cmp_greater_equal(x, canvas.width) ||
        std::cmp_greater_equal(y, canvas.height)) {
        return;
    }
    u8* pixel =
        canvas.pixels + (((static_cast<usize>(y) * canvas.width) + static_cast<usize>(x)) * 4);
    for (u32 channel = 0; channel < 3; ++channel) {
        const u32 shift = 16U - (channel * 8U);
        const f32 source = static_cast<f32>((colour >> shift) & 0xFFU);
        const f32 destination = static_cast<f32>(pixel[channel]);
        pixel[channel] =
            static_cast<u8>(std::lround((source * alpha) + (destination * (1.0F - alpha))));
    }
}

/// The span of row centre `y` inside a convex polygon, as [left, right).
[[nodiscard]] bool row_span(Span<const Vec2> corners, f32 y, f32& left, f32& right) noexcept {
    left = 0.0F;
    right = 0.0F;
    bool found = false;
    for (usize index = 0; index < corners.size(); ++index) {
        const Vec2 a = corners[index];
        const Vec2 b = corners[(index + 1) % corners.size()];
        const f32 low = std::min(a.y, b.y);
        const f32 high = std::max(a.y, b.y);
        if (a.y == b.y || y < low || y >= high) {
            continue;
        }
        const f32 x = a.x + ((y - a.y) * (b.x - a.x) / (b.y - a.y));
        left = found ? std::min(left, x) : x;
        right = found ? std::max(right, x) : x;
        found = true;
    }
    return found;
}

/// Clips the segment to `[low, high]` in both axes (Liang-Barsky). False when nothing is left.
[[nodiscard]] bool clip_segment(Vec2& from, Vec2& to, Vec2 low, Vec2 high) noexcept {
    const Vec2 delta{to.x - from.x, to.y - from.y};
    const f32 p[4] = {-delta.x, delta.x, -delta.y, delta.y};
    const f32 q[4] = {from.x - low.x, high.x - from.x, from.y - low.y, high.y - from.y};
    f32 enter = 0.0F;
    f32 leave = 1.0F;
    for (u32 edge = 0; edge < 4; ++edge) {
        if (p[edge] == 0.0F) {
            if (q[edge] < 0.0F) {
                return false;
            }
            continue;
        }
        const f32 t = q[edge] / p[edge];
        if (p[edge] < 0.0F) {
            enter = std::max(enter, t);
        } else {
            leave = std::min(leave, t);
        }
    }
    if (enter > leave) {
        return false;
    }
    to = Vec2{from.x + (delta.x * leave), from.y + (delta.y * leave)};
    from = Vec2{from.x + (delta.x * enter), from.y + (delta.y * enter)};
    return true;
}

void stamp(const Canvas& canvas, Vec2 at, u32 colour, i32 radius) noexcept {
    const i32 x = static_cast<i32>(std::floor(at.x));
    const i32 y = static_cast<i32>(std::floor(at.y));
    for (i32 dy = -radius; dy <= radius; ++dy) {
        for (i32 dx = -radius; dx <= radius; ++dx) {
            blend(canvas, x + dx, y + dy, colour, kLineAlpha);
        }
    }
}

void draw_line(const Canvas& canvas, Vec2 from, Vec2 to, u32 colour, f32 width) noexcept {
    const f32 margin = width + 1.0F;
    if (!clip_segment(from, to, Vec2{-margin, -margin},
                      Vec2{static_cast<f32>(canvas.width) + margin,
                           static_cast<f32>(canvas.height) + margin})) {
        return;
    }
    const f32 span = std::max(std::fabs(to.x - from.x), std::fabs(to.y - from.y));
    const f32 steps = std::min(std::ceil(span), kMaxLineSteps);
    const i32 radius = static_cast<i32>(width * 0.5F);
    const u32 count = static_cast<u32>(steps);
    for (u32 step = 0; step <= count; ++step) {
        const f32 t = count == 0 ? 0.0F : static_cast<f32>(step) / steps;
        stamp(canvas, Vec2{from.x + ((to.x - from.x) * t), from.y + ((to.y - from.y) * t)}, colour,
              radius);
    }
}

[[nodiscard]] bool project(const NavOverlayView& view, Vec3 point, Vec2& out) noexcept {
    return render::project_to_pixel(view.view, point - view.eye, out);
}

void draw_path(NavCanvasSink& sink, Span<const Vec3> path) noexcept {
    for (usize index = 1; index < path.size(); ++index) {
        sink.path_segment(path[index - 1], path[index]);
    }
}

void draw_flow(NavCanvasSink& sink, const NavFlowOverlay& flow) noexcept {
    const usize cells = static_cast<usize>(flow.width) * flow.depth;
    if (cells == 0 || flow.directions.size() < cells || flow.reachable.size() < cells) {
        return;
    }
    const f32 y = flow.region.min.y + ((flow.region.max.y - flow.region.min.y) * 0.5F);
    for (u32 row = 0; row < flow.depth; ++row) {
        for (u32 column = 0; column < flow.width; ++column) {
            const usize index = (static_cast<usize>(row) * flow.width) + column;
            const Vec3 centre{flow.region.min.x + ((static_cast<f32>(column) + 0.5F) * flow.cell),
                              y, flow.region.min.z + ((static_cast<f32>(row) + 0.5F) * flow.cell)};
            sink.flow_arrow(centre, flow.directions[index], flow.cell * 0.4F,
                            flow.reachable[index] != 0);
        }
    }
}

}  // namespace

u32 nav_area_colour(nav::AreaType area) noexcept {
    if (area == nav::kAreaGround) {
        return kGround;
    }
    if (area == nav::kAreaNull) {
        return kCarved;
    }
    return kAreaPalette[area % 8U];
}

void fill_convex_polygon(const Canvas& canvas, Span<const Vec2> corners, u32 colour,
                         f32 alpha) noexcept {
    if (corners.size() < 3) {
        return;
    }
    f32 top = corners[0].y;
    f32 bottom = corners[0].y;
    for (const Vec2 corner : corners) {
        top = std::min(top, corner.y);
        bottom = std::max(bottom, corner.y);
    }
    // Rows and columns are clamped to the canvas in float before they become integers, so a
    // polygon projected far off the frame costs nothing and cannot overflow.
    const f32 last_row = static_cast<f32>(canvas.height) - 1.0F;
    const auto first = static_cast<i32>(std::max(0.0F, std::ceil(top - 0.5F)));
    const auto last = static_cast<i32>(std::min(last_row, std::ceil(bottom - 0.5F) - 1.0F));
    for (i32 row = first; row <= last; ++row) {
        f32 left = 0.0F;
        f32 right = 0.0F;
        if (!row_span(corners, static_cast<f32>(row) + 0.5F, left, right)) {
            continue;
        }
        const auto from = static_cast<i32>(std::max(0.0F, std::ceil(left - 0.5F)));
        const auto to =
            static_cast<i32>(std::min(static_cast<f32>(canvas.width), std::ceil(right - 0.5F)));
        for (i32 column = from; column < to; ++column) {
            blend(canvas, column, row, colour, alpha);
        }
    }
}

void NavCanvasSink::polygon(nav::PolyRef, Span<const Vec3> corners, nav::AreaType area) noexcept {
    Vec2 projected[nav::kMaxPolyVertices] = {};
    const usize count = std::min<usize>(corners.size(), nav::kMaxPolyVertices);
    for (usize index = 0; index < count; ++index) {
        if (!project(view_, corners[index], projected[index])) {
            return;  // a corner behind the camera: the polygon has no answer on this frame
        }
    }
    fill_convex_polygon(canvas_, Span<const Vec2>(projected, count), nav_area_colour(area),
                        kPolygonAlpha);
    polygons_drawn_ += 1;
}

void NavCanvasSink::tile(nav::TileCoord, Aabb bounds) noexcept {
    const f32 y = bounds.min.y;
    const Vec3 corners[4] = {
        Vec3{bounds.min.x, y, bounds.min.z}, Vec3{bounds.max.x, y, bounds.min.z},
        Vec3{bounds.max.x, y, bounds.max.z}, Vec3{bounds.min.x, y, bounds.max.z}};
    for (u32 index = 0; index < 4; ++index) {
        line(corners[index], corners[(index + 1) % 4], kTileBorder, 1.0F);
    }
}

void NavCanvasSink::link(nav::LinkId, Vec3 from, Vec3 to, Name) noexcept {
    line(from, to, kLink, 2.0F);
    Vec2 end;
    if (project(view_, from, end)) {
        stamp(canvas_, end, kLink, 2);
    }
    if (project(view_, to, end)) {
        stamp(canvas_, end, kLink, 2);
    }
}

void NavCanvasSink::path_segment(Vec3 from, Vec3 to) noexcept {
    line(from, to, kPath, 3.0F);
}

void NavCanvasSink::obstacle(nav::ObstacleId, const nav::NavObstacleShape& shape) noexcept {
    if (shape.radius > 0.0F) {
        constexpr u32 kSides = 24;
        constexpr f32 kTurn = 6.2831853F;
        for (u32 side = 0; side < kSides; ++side) {
            const f32 a = kTurn * static_cast<f32>(side) / static_cast<f32>(kSides);
            const f32 b = kTurn * static_cast<f32>(side + 1) / static_cast<f32>(kSides);
            line(shape.centre + Vec3{std::cos(a) * shape.radius, 0.0F, std::sin(a) * shape.radius},
                 shape.centre + Vec3{std::cos(b) * shape.radius, 0.0F, std::sin(b) * shape.radius},
                 kObstacle, 2.0F);
        }
        return;
    }
    const Aabb box = shape.bounds;
    const Vec3 corners[4] = {
        Vec3{box.min.x, box.min.y, box.min.z}, Vec3{box.max.x, box.min.y, box.min.z},
        Vec3{box.max.x, box.min.y, box.max.z}, Vec3{box.min.x, box.min.y, box.max.z}};
    for (u32 index = 0; index < 4; ++index) {
        line(corners[index], corners[(index + 1) % 4], kObstacle, 2.0F);
    }
}

void NavCanvasSink::flow_arrow(Vec3 centre, Vec2 direction, f32 length, bool reachable) noexcept {
    const u32 colour = reachable ? kFlowReachable : kFlowBlocked;
    const Vec3 tip = centre + Vec3{direction.x * length, 0.0F, direction.y * length};
    line(centre, tip, colour, 1.0F);
    // The head: two short strokes back from the tip, a third of the shaft long.
    const Vec3 back{-direction.x * length * 0.35F, 0.0F, -direction.y * length * 0.35F};
    const Vec3 side{-direction.y * length * 0.2F, 0.0F, direction.x * length * 0.2F};
    line(tip, tip + back + side, colour, 1.0F);
    line(tip, tip + back - side, colour, 1.0F);
}

void NavCanvasSink::line(Vec3 from, Vec3 to, u32 colour, f32 width) noexcept {
    Vec2 a;
    Vec2 b;
    if (project(view_, from, a) && project(view_, to, b)) {
        draw_line(canvas_, a, b, colour, width);
    }
}

void draw_navigation_overlay(const Canvas& canvas, const NavOverlayView& view,
                             const NavOverlayWorld& world) noexcept {
    if (world.flags == 0 || world.mesh == nullptr) {
        return;
    }
    NavCanvasSink sink(canvas, view);
    const auto flags = static_cast<nav::NavDebugFlags>(world.flags);
    nav::draw_navigation_mesh(*world.mesh, flags, sink);
    if (!nav::has_flag(flags, nav::NavDebugFlags::Paths)) {
        return;
    }
    draw_path(sink, world.path);
    if (world.flow != nullptr) {
        draw_flow(sink, *world.flow);
    }
}

void draw_navigation_overlays(const Canvas& canvas, const NavOverlayView& view,
                              Span<const NavOverlayWorld> worlds) noexcept {
    for (const NavOverlayWorld& world : worlds) {
        draw_navigation_overlay(canvas, view, world);
    }
}

}  // namespace cy::sample::editor_window
