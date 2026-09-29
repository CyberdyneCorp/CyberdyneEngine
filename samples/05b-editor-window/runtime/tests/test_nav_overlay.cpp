// SPDX-License-Identifier: MIT
// The navigation overlay against the polygons it draws, on the CPU canvas with no device. Issue
// #28, task 3.3 and acceptance criterion 2 of `implement-issue-28-navigation-authoring`.
//
// A known mesh (a baked 8 m square of ground), a known view (straight down from 12 m) and a
// cleared canvas: the pixels the overlay covers must be the pixels whose centres lie inside the
// walkable polygons projected through the same view, and their count must match the projected
// polygons' area. The overlay is not depth tested, so nothing else in a frame could hide a pixel
// this compares.

#include <cy/core/memory/system_allocator.h>
#include <cy/navigation/bake.h>
#include <cy/navigation/debug.h>
#include <cy/servers/render/picking.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "nav_fixture.h"
#include "nav_overlay.h"
#include "nav_test_support.h"

using namespace cy;
using namespace cy::sample::editor_window;
using cy::sample::editor_window::testing::overhead_view;
namespace nav = cy::navigation;

namespace {

[[nodiscard]] Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

constexpr u32 kSide = 96;

/// An RGBA canvas cleared to zero.
struct Frame {
    std::vector<u8> pixels = std::vector<u8>(static_cast<usize>(kSide) * kSide * 4, u8{0});
    [[nodiscard]] Canvas canvas() { return Canvas{pixels.data(), kSide, kSide}; }
    [[nodiscard]] bool covered(u32 x, u32 y) const {
        const usize at = ((static_cast<usize>(y) * kSide) + x) * 4;
        return pixels[at] != 0 || pixels[at + 1] != 0 || pixels[at + 2] != 0;
    }
};

[[nodiscard]] nav::NavBakeSettings settings() noexcept {
    nav::NavBakeSettings out;
    out.backend = nav::NavBuildBackend::Engine;
    out.cell_size = 0.25F;
    out.cell_height = 0.2F;
    out.agent_radius = 0.25F;
    out.tile_size = 8.0F;
    return out;
}

/// A baked square of ground from `x0` along x, one 8 m tile per 8 m.
[[nodiscard]] nav::NavMesh baked_square(f32 x0, u32 metres) {
    nav::testing::SourceGeometry geometry(allocator());
    geometry.ground(x0, 0.0F, metres, 8, 1.0F);
    const nav::NavSurfaceVolume surface{
        1,
        Aabb::from_min_max(Vec3{x0, -1.0F, 0.0F}, Vec3{x0 + static_cast<f32>(metres), 3.0F, 8.0F}),
        false};
    nav::NavBakeSource source;
    source.geometry = geometry.geometry();
    source.surfaces = Span<const nav::NavSurfaceVolume>(&surface, 1);
    nav::NavMesh mesh(allocator(), Name::intern("overlay"), 8.0F);
    auto report = nav::bake_tiles(allocator(), settings(), source, surface.bounds, mesh, nullptr);
    CY_REQUIRE(report.has_value());
    return mesh;
}

/// Every polygon the mesh's debug stream reports, as world-space corners.
class Collect final : public nav::NavDebugSink {
public:
    void polygon(nav::PolyRef, Span<const Vec3> corners, nav::AreaType) noexcept override {
        polygons.emplace_back(corners.begin(), corners.end());
    }
    std::vector<std::vector<Vec3>> polygons;
};

[[nodiscard]] std::vector<std::vector<Vec2>> projected_polygons(const nav::NavMesh& mesh,
                                                                const NavOverlayView& view) {
    Collect collect;
    nav::draw_navigation_mesh(mesh, nav::NavDebugFlags::Polygons, collect);
    std::vector<std::vector<Vec2>> out;
    for (const std::vector<Vec3>& corners : collect.polygons) {
        std::vector<Vec2> pixels;
        for (const Vec3 corner : corners) {
            Vec2 pixel;
            CY_REQUIRE(render::project_to_pixel(view.view, corner - view.eye, pixel));
            pixels.push_back(pixel);
        }
        out.push_back(std::move(pixels));
    }
    return out;
}

/// Whether `point` lies inside a convex polygon of either winding.
[[nodiscard]] bool inside(const std::vector<Vec2>& polygon, Vec2 point) {
    bool positive = false;
    bool negative = false;
    for (usize index = 0; index < polygon.size(); ++index) {
        const Vec2 a = polygon[index];
        const Vec2 b = polygon[(index + 1) % polygon.size()];
        const f32 side = ((b.x - a.x) * (point.y - a.y)) - ((b.y - a.y) * (point.x - a.x));
        positive = positive || side > 0.0F;
        negative = negative || side < 0.0F;
    }
    return !(positive && negative);
}

/// A polygon's pixel bounds. The per-pixel scans test a polygon only when its bounds hold the
/// pixel centre, which keeps a Debug build inside the case budget.
struct Bounds {
    Vec2 low{};
    Vec2 high{};
};

[[nodiscard]] Bounds bounds_of(const std::vector<Vec2>& polygon) {
    Bounds out;
    if (polygon.empty()) {
        return out;
    }
    out.low = polygon.front();
    out.high = polygon.front();
    for (const Vec2 corner : polygon) {
        out.low = Vec2{std::min(out.low.x, corner.x), std::min(out.low.y, corner.y)};
        out.high = Vec2{std::max(out.high.x, corner.x), std::max(out.high.y, corner.y)};
    }
    return out;
}

[[nodiscard]] bool within(const Bounds& bounds, Vec2 point) {
    return point.x >= bounds.low.x && point.x <= bounds.high.x && point.y >= bounds.low.y &&
           point.y <= bounds.high.y;
}

[[nodiscard]] bool inside_any(const std::vector<std::vector<Vec2>>& polygons,
                              const std::vector<Bounds>& bounds, Vec2 point) {
    for (usize index = 0; index < polygons.size(); ++index) {
        if (within(bounds[index], point) && inside(polygons[index], point)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] f64 shoelace(const std::vector<Vec2>& polygon) {
    f64 twice = 0.0;
    for (usize index = 0; index < polygon.size(); ++index) {
        const Vec2 a = polygon[index];
        const Vec2 b = polygon[(index + 1) % polygon.size()];
        twice += (static_cast<f64>(a.x) * static_cast<f64>(b.y)) -
                 (static_cast<f64>(b.x) * static_cast<f64>(a.y));
    }
    return std::fabs(twice) * 0.5;
}

struct Coverage {
    u32 expected = 0;
    u32 drawn = 0;
    u32 mismatched = 0;
    /// Pixels both expected and drawn.
    u32 hit = 0;
};

void count(Coverage& coverage, bool expected, bool drawn) {
    coverage.expected += expected ? 1U : 0U;
    coverage.drawn += drawn ? 1U : 0U;
    coverage.mismatched += expected != drawn ? 1U : 0U;
    coverage.hit += expected && drawn ? 1U : 0U;
}

[[nodiscard]] Coverage compare(const Frame& frame, const std::vector<std::vector<Vec2>>& polygons) {
    Coverage out;
    std::vector<Bounds> bounds;
    bounds.reserve(polygons.size());
    for (const std::vector<Vec2>& polygon : polygons) {
        bounds.push_back(bounds_of(polygon));
    }
    for (u32 y = 0; y < kSide; ++y) {
        for (u32 x = 0; x < kSide; ++x) {
            const Vec2 centre{static_cast<f32>(x) + 0.5F, static_cast<f32>(y) + 0.5F};
            count(out, inside_any(polygons, bounds, centre), frame.covered(x, y));
        }
    }
    return out;
}

}  // namespace

CY_TEST_CASE("nav overlay covers the projected walkable polygons") {
    const nav::NavMesh mesh = baked_square(0.0F, 8);
    CY_REQUIRE(mesh.stats().polys > 0U);
    const NavOverlayView view = overhead_view(Vec3{4.0F, 12.0F, 4.0F}, kSide, kSide);
    const std::vector<std::vector<Vec2>> polygons = projected_polygons(mesh, view);
    CY_REQUIRE_FALSE(polygons.empty());

    Frame frame;
    const NavOverlayWorld world{
        1, &mesh, static_cast<u32>(nav::NavDebugFlags::Polygons), {}, nullptr};
    draw_navigation_overlay(frame.canvas(), view, world);

    const Coverage coverage = compare(frame, polygons);
    // The walkable square fills a large part of the frame and not all of it.
    CY_CHECK_GT(coverage.expected, (kSide * kSide) / 4U);
    CY_CHECK_LT(coverage.expected, kSide * kSide);
    // Pixel for pixel: only pixels whose centre sits on a shared edge may disagree.
    CY_CHECK_LE(coverage.mismatched, coverage.expected / 100U);
    // And the covered area is the projected polygons' area, independently of the fill rule.
    f64 area = 0.0;
    for (const std::vector<Vec2>& polygon : polygons) {
        area += shoelace(polygon);
    }
    CY_CHECK_NEAR(static_cast<f64>(coverage.drawn), area, area * 0.02);
}

CY_TEST_CASE("nav overlay per-world toggle draws only the enabled world") {
    const nav::NavMesh left = baked_square(0.0F, 8);
    const nav::NavMesh right = baked_square(10.0F, 8);
    const NavOverlayView view = overhead_view(Vec3{9.0F, 16.0F, 4.0F}, kSide, kSide);
    const std::vector<std::vector<Vec2>> left_polygons = projected_polygons(left, view);
    const std::vector<std::vector<Vec2>> right_polygons = projected_polygons(right, view);

    const u32 polygons = static_cast<u32>(nav::NavDebugFlags::Polygons);
    NavOverlayWorld worlds[2] = {{1, &left, polygons, {}, nullptr}, {2, &right, 0U, {}, nullptr}};
    Frame only_left;
    draw_navigation_overlays(only_left.canvas(), view, Span<const NavOverlayWorld>(worlds, 2));
    const Coverage left_drawn = compare(only_left, left_polygons);
    const Coverage right_hidden = compare(only_left, right_polygons);
    CY_CHECK_GT(left_drawn.expected, 0U);
    CY_CHECK_LE(left_drawn.mismatched, left_drawn.expected / 50U);
    // Nothing of the disabled world is drawn.
    CY_CHECK_GT(right_hidden.expected, 0U);
    CY_CHECK_EQ(right_hidden.hit, 0U);

    worlds[1].flags = polygons;
    Frame both;
    draw_navigation_overlays(both.canvas(), view, Span<const NavOverlayWorld>(worlds, 2));
    const Coverage right_drawn = compare(both, right_polygons);
    CY_CHECK_GE(right_drawn.hit + (right_drawn.expected / 50U), right_drawn.expected);
    CY_CHECK_GT(right_drawn.drawn, left_drawn.drawn);
}

CY_TEST_CASE("nav overlay draws tiles, links, obstacles, a path and flow arrows inside the frame") {
    nav::NavMesh mesh = baked_square(0.0F, 8);
    nav::NavObstacleShape pillar;
    pillar.centre = Vec3{2.0F, 0.0F, 2.0F};
    pillar.radius = 0.75F;
    pillar.height = 2.0F;
    CY_REQUIRE(mesh.add_obstacle(pillar).has_value());
    const NavOverlayView view = overhead_view(Vec3{4.0F, 12.0F, 4.0F}, kSide, kSide);

    const std::vector<Vec3> path = {Vec3{1.0F, 0.0F, 7.0F}, Vec3{7.0F, 0.0F, 7.0F},
                                    Vec3{400.0F, 0.0F, -400.0F}};
    NavFlowOverlay flow;
    flow.region = Aabb::from_min_max(Vec3{4.0F, 0.0F, 4.0F}, Vec3{8.0F, 0.0F, 8.0F});
    flow.cell = 1.0F;
    flow.width = 4;
    flow.depth = 4;
    for (u32 cell = 0; cell < 16; ++cell) {
        CY_REQUIRE(flow.directions.push_back(Vec2{1.0F, 0.0F}).has_value());
        CY_REQUIRE(flow.reachable.push_back(u8{1}).has_value());
    }
    const u32 lines = static_cast<u32>(nav::NavDebugFlags::Tiles) |
                      static_cast<u32>(nav::NavDebugFlags::Obstacles) |
                      static_cast<u32>(nav::NavDebugFlags::Paths);
    Frame frame;
    const NavOverlayWorld world{1, &mesh, lines, Span<const Vec3>(path.data(), path.size()), &flow};
    draw_navigation_overlay(frame.canvas(), view, world);

    // No polygon was asked for, so what is drawn is lines: some pixels, far from all of them. A
    // path running hundreds of metres off the frame is clipped rather than walked or written.
    const Coverage coverage = compare(frame, {});
    CY_CHECK_GT(coverage.drawn, 100U);
    CY_CHECK_LT(coverage.drawn, (kSide * kSide) / 2U);
    // The pillar footprint's rim is drawn at its projected radius.
    Vec2 rim;
    CY_REQUIRE(render::project_to_pixel(view.view, Vec3{2.75F, 0.0F, 2.0F} - view.eye, rim));
    // The rim must land inside the frame, with a column either side to look at.
    const bool framed = rim.x >= 1.0F && rim.y >= 0.0F && rim.x < static_cast<f32>(kSide - 1) &&
                        rim.y < static_cast<f32>(kSide);
    CY_REQUIRE(framed);
    if (!framed) {
        return;  // `x - 1` would wrap on a u32 in this exception-free build
    }
    const u32 x = static_cast<u32>(rim.x);
    const u32 y = static_cast<u32>(rim.y);
    const bool rim_drawn =
        frame.covered(x, y) || frame.covered(x - 1, y) || frame.covered(x + 1, y);
    CY_CHECK(rim_drawn);
}

CY_TEST_CASE("nav overlay clips a polygon at the near plane with the camera inside the tile") {
    // An author's view: 1.5 m above the middle of a tile, looking north and down. A navmesh
    // polygon may span the whole tile (Recast merges cells into large convex polygons), so this
    // one does: two of its corners are behind the camera and have no projection.
    NavOverlayView view = overhead_view(Vec3{4.0F, 1.5F, 4.0F}, kSide, kSide);
    const Vec3 forward = normalize(Vec3{0.0F, -0.6F, -1.0F});
    const Vec3 right = normalize(cross(forward, Vec3{0.0F, 1.0F, 0.0F}));
    const Vec3 up = cross(right, forward);
    view.view.desc.camera.rotation = Quat::from_basis(right, up, -forward);
    view.view.refresh();
    const Vec3 square[4] = {Vec3{0.0F, 0.0F, 0.0F}, Vec3{8.0F, 0.0F, 0.0F}, Vec3{8.0F, 0.0F, 8.0F},
                            Vec3{0.0F, 0.0F, 8.0F}};
    Vec2 ignored;
    CY_REQUIRE_FALSE(render::project_to_pixel(view.view, square[2] - view.eye, ignored));

    Frame frame;
    NavCanvasSink sink(frame.canvas(), view);
    sink.polygon(nav::PolyRef{}, Span<const Vec3>(square, 4), nav::kAreaGround);
    CY_CHECK_EQ(sink.polygons_drawn(), 1U);

    // The reference is independent of the projection: each pixel's ray meets the ground plane,
    // and the pixel is expected covered when that point lies inside the square.
    Coverage coverage;
    for (u32 y = 0; y < kSide; ++y) {
        for (u32 x = 0; x < kSide; ++x) {
            const Ray ray = render::ray_through_pixel(view.view, static_cast<f32>(x) + 0.5F,
                                                      static_cast<f32>(y) + 0.5F);
            const Vec3 origin = ray.origin + view.eye;
            bool expected = false;
            if (ray.direction.y < -1e-4F) {
                const Vec3 hit = origin + (ray.direction * (-origin.y / ray.direction.y));
                expected = hit.x > 0.0F && hit.x < 8.0F && hit.z > 0.0F && hit.z < 8.0F;
            }
            count(coverage, expected, frame.covered(x, y));
        }
    }
    // The ground in front of the camera fills the lower part of the frame.
    CY_CHECK_GT(coverage.expected, (kSide * kSide) / 4U);
    CY_CHECK_LE(coverage.mismatched, coverage.expected / 100U);

    // A polygon wholly behind the camera draws nothing.
    const Vec3 behind[3] = {Vec3{3.0F, 0.0F, 6.0F}, Vec3{5.0F, 0.0F, 6.0F}, Vec3{4.0F, 0.0F, 7.0F}};
    Frame empty;
    NavCanvasSink hidden(empty.canvas(), view);
    hidden.polygon(nav::PolyRef{}, Span<const Vec3>(behind, 3), nav::kAreaGround);
    CY_CHECK_EQ(hidden.polygons_drawn(), 0U);
    CY_CHECK_EQ(compare(empty, {}).drawn, 0U);
}
