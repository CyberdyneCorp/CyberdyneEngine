// SPDX-License-Identifier: MIT
// cy/abi/game/camera.h — the backend behind ABI 1.3's `camera_*` entries. `add-swift-game-api`.
//
// OWNER: implementer A (input and camera). See openspec/changes/add-swift-game-api/design.md.
//
// The camera is presentation. Reads are refused in a fixed step by their thunks; writes are allowed
// there, and their thunks drop them while the tick is a resimulation, so a backend never sees a
// write it would have to undo. The projections must be `cy::camera::screen_point_to_ray` and
// `cy::camera::world_to_screen` over the camera's evaluated state and its view's viewport — one
// projection model, not a second one here.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::abi::game {

/// The camera server as ABI 1.3's `camera_*` entries see it: the primary view's camera, its
/// projections, and the rig's target and pose override.
class CameraBackend {
public:
    virtual ~CameraBackend() = default;

    /// `camera_active`: the camera of the primary view; UNAVAILABLE when there is none.
    [[nodiscard]] virtual CyResult active(CyCamera& out_camera) noexcept = 0;

    /// `camera_view`: `out_view` arrives zeroed with `struct_size == sizeof`. NOT_FOUND for a stale
    /// or unknown camera.
    [[nodiscard]] virtual CyResult view(CyCamera camera, CyCameraView& out_view) noexcept = 0;

    /// `camera_screen_to_ray`: `max_distance` is the near-to-far span.
    [[nodiscard]] virtual CyResult screen_to_ray(CyCamera camera, f32 screen_x, f32 screen_y,
                                                 CyRay& out_ray) noexcept = 0;

    /// `camera_world_to_screen`: `points_xyz.size() == 3 * out_points.size()`, checked by the
    /// thunk.
    [[nodiscard]] virtual CyResult world_to_screen(CyCamera camera, Span<const f32> points_xyz,
                                                   Span<CyScreenPoint> out_points) noexcept = 0;

    /// `camera_set_target`: `target` is a whole struct.
    [[nodiscard]] virtual CyResult set_target(CyCamera camera,
                                              const CyCameraTarget& target) noexcept = 0;

    /// `camera_set_pose` and `camera_clear_pose`: the rig's pose override.
    [[nodiscard]] virtual CyResult set_pose(CyCamera camera, const CyPose& pose) noexcept = 0;
    [[nodiscard]] virtual CyResult clear_pose(CyCamera camera) noexcept = 0;
};

}  // namespace cy::abi::game
