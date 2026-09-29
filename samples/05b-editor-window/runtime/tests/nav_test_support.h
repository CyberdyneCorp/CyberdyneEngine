// SPDX-License-Identifier: MIT
#pragma once
// What the navigation overlay and runtime suites share: a view looking straight down, built the
// way the runtime builds the view of a frame (camera-relative, so the eye is carried separately).
// Issue #28, tasks 3.3 to 3.6.

#include <cy/core/math/quat.h>
#include <cy/core/math/vec.h>
#include <cy/servers/render/model.h>

#include "nav_overlay.h"

namespace cy::sample::editor_window::testing {

inline constexpr f32 kFieldOfView = 0.9F;

/// A perspective view from `eye` looking down -Y, with -Z at the top of the frame.
[[nodiscard]] inline NavOverlayView overhead_view(Vec3 eye, u32 width, u32 height) noexcept {
    NavOverlayView out;
    render::View& view = out.view;
    view.desc.purpose = render::ViewPurpose::EditorViewport;
    view.desc.viewport = render::ViewportRect{0, 0, width, height};
    view.desc.projection.kind = render::ProjectionKind::Perspective;
    view.desc.projection.fov_y_radians = kFieldOfView;
    view.desc.projection.near_plane = 0.1F;
    view.desc.projection.far_plane = 0.0F;
    const Vec3 forward{0.0F, -1.0F, 0.0F};
    const Vec3 right = normalize(cross(forward, Vec3{0.0F, 0.0F, -1.0F}));
    const Vec3 up = cross(right, forward);
    view.desc.camera.rotation = Quat::from_basis(right, up, -forward);
    view.desc.camera.translation = Vec3{0.0F, 0.0F, 0.0F};
    view.refresh();
    out.eye = eye;
    return out;
}

}  // namespace cy::sample::editor_window::testing
