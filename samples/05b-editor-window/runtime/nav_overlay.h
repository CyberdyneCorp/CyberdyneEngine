// SPDX-License-Identifier: MIT
#pragma once
// The navigation overlay, rasterised by the engine into the frame it publishes. Issue #28, task
// 3.3 of `implement-issue-28-navigation-authoring`.
//
// `NavCanvasSink` is a `cy::navigation::NavDebugSink` over the runtime's CPU `Canvas`: the engine's
// renderer-neutral debug stream (`draw_navigation_mesh`) says what to draw, and this file only
// projects it with the frame's own view (`cy::render::project_to_pixel`) and fills pixels. It draws
//
//   - walkable polygons, filled in their EFFECTIVE area's colour, so an obstacle-marked polygon
//     shows in the colour of the area the obstacle marks it with;
//   - tile borders, off-mesh links and obstacle footprints as lines;
//   - the current test path as a thick polyline and, when one was requested, flow-field arrows.
//
// Like the gizmo in `overlay.h`, the overlay is composited over the lit frame and is NOT depth
// tested: it is drawn on top of every surface, which is what a navmesh debug view does in every
// editor and what the image test (`test_nav_overlay.cpp`) relies on to compare the covered pixels
// with the projected polygons. It is a per-world toggle rather than a `DebugViewMode`.

#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/navigation/debug.h>
#include <cy/navigation/navmesh.h>
#include <cy/servers/render/model.h>

#include "overlay.h"

namespace cy::sample::editor_window {

/// The view a frame was rendered with. `view` is camera-relative, as the renderer's is, so a world
/// point is projected as `point - eye`.
struct NavOverlayView {
    render::View view;
    Vec3 eye;
};

/// A flow field the editor asked for, as `navigation.flowfield.query` answered it.
struct NavFlowOverlay {
    Aabb region;
    f32 cell = 0.0F;
    u32 width = 0;
    u32 depth = 0;
    /// Per cell, row by row: the XZ direction toward the target, and whether the cell reaches it.
    Array<Vec2> directions;
    Array<u8> reachable;
};

/// One navigation world as the frame draws it.
struct NavOverlayWorld {
    u32 world = 0;
    const navigation::NavMesh* mesh = nullptr;
    /// `navigation::NavDebugFlags` bits. Zero draws nothing: the world's overlay is off.
    u32 flags = 0;
    /// The last test path's points, drawn when `flags` has `Paths`.
    Span<const Vec3> path;
    /// The last flow field, drawn when `flags` has `Paths`. May be null.
    const NavFlowOverlay* flow = nullptr;
};

/// The RGB colour an area is drawn in, packed as 0x00RRGGBB (the convention `overlay.cpp` uses).
/// Ground is cyan, a carved-out
/// (`kAreaNull`) footprint red, and every other area takes a stable colour of its own.
[[nodiscard]] u32 nav_area_colour(navigation::AreaType area) noexcept;

/// Fills a convex polygon given in frame pixels. A pixel is covered when its centre lies inside
/// the polygon. Exposed for the image test.
void fill_convex_polygon(const Canvas& canvas, Span<const Vec2> corners, u32 colour,
                         f32 alpha) noexcept;

/// The debug sink that rasterises onto a canvas.
class NavCanvasSink final : public navigation::NavDebugSink {
public:
    NavCanvasSink(const Canvas& canvas, const NavOverlayView& view) noexcept
        : canvas_(canvas), view_(view) {}

    void polygon(navigation::PolyRef ref, Span<const Vec3> corners,
                 navigation::AreaType area) noexcept override;
    void tile(navigation::TileCoord coord, Aabb bounds) noexcept override;
    void link(navigation::LinkId id, Vec3 from, Vec3 to, Name action) noexcept override;
    void path_segment(Vec3 from, Vec3 to) noexcept override;
    void obstacle(navigation::ObstacleId id,
                  const navigation::NavObstacleShape& shape) noexcept override;

    /// A flow-field cell's arrow, from `centre` along the XZ `direction`.
    void flow_arrow(Vec3 centre, Vec2 direction, f32 length, bool reachable) noexcept;

    /// How many polygons were filled. A polygon partly behind the camera is clipped to the near
    /// plane and filled; one wholly behind it is skipped.
    [[nodiscard]] u32 polygons_drawn() const noexcept { return polygons_drawn_; }

private:
    void line(Vec3 from, Vec3 to, u32 colour, f32 width) noexcept;

    Canvas canvas_;
    NavOverlayView view_;
    u32 polygons_drawn_ = 0;
};

/// Draws one world: its mesh through `draw_navigation_mesh` with the world's flags, then its test
/// path and flow field. Does nothing when the flags are zero or there is no mesh.
void draw_navigation_overlay(const Canvas& canvas, const NavOverlayView& view,
                             const NavOverlayWorld& world) noexcept;

/// Draws every world whose overlay is enabled, in order.
void draw_navigation_overlays(const Canvas& canvas, const NavOverlayView& view,
                              Span<const NavOverlayWorld> worlds) noexcept;

}  // namespace cy::sample::editor_window
