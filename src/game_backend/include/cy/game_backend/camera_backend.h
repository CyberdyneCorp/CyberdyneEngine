// SPDX-License-Identifier: MIT
// cy/game_backend/camera_backend.h — the `camera` adapter behind ABI 1.3. `add-swift-game-api`.
//
// OWNER: implementer A. The contract is cy/abi/game/camera.h and design.md.
//
// `CameraAdapter` answers the `camera_*` entries from a `cy::camera::CameraServer`. A `CyCamera` is
// a `RigHandle`'s bits, so a destroyed rig answers NOT_FOUND through the server's own generation
// check and no table of cameras lives here.
//
//   * THE PRIMARY VIEW is what the host declares with `set_primary_view`: the rig the frame's main
//     view is evaluated from, and the `RenderViewRequest` it produces that view with. The reads —
//     `camera_view` and the two projections — produce the view from the rig's LAST EVALUATION with
//     `cy::camera::produce_view` and project with `cy::camera::screen_point_to_ray` and
//     `cy::camera::world_to_screen`. There is one projection model and this is not a second one.
//     A camera other than the primary one has no viewport here, so its reads are NOT_FOUND.
//   * THE WRITES go to the rig. `camera_set_pose` / `camera_clear_pose` are the server's
//     `override_pose` / `clear_pose_override`. `camera_set_target` binds the focus with
//     `CameraServer::set_target` (an entity is bound by its `CyEntity` value as the stable id,
//     which the host resolves into a `TargetSample` as it does for every binding), cuts when
//     `blend_seconds` is zero, and records the requested yaw, pitch and distance as the rig's
//     `Framing`. The server has intent verbs for an orbit — look deltas and a zoom — and no verb
//     for an absolute one, so the host's rig reads `framing()` rather than this adapter inventing a
//     conversion the rig graph would then fight.
//
// Not thread-safe, like the server: the game thread only.

#pragma once

#include <cy/abi/game/camera.h>
#include <cy/abi/host.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/camera/server.h>

namespace cy::game_backend {

/// Implements `cy::abi::game::CameraBackend` over a `cy::camera::CameraServer`.
class CameraAdapter final : public cy::abi::game::CameraBackend {
public:
    /// What `camera_set_target` last asked of one rig, beyond the focus the server binds.
    struct Framing {
        f32 yaw = 0.0F;
        f32 pitch = 0.0F;
        f32 distance = 0.0F;
        f32 blend_seconds = 0.0F;
        /// Incremented by every accepted `camera_set_target`, so a host can tell a new request
        /// from the one it already applied.
        u32 revision = 0;
    };

    CameraAdapter(cy::camera::CameraServer& server, Allocator& allocator) noexcept
        : server_(&server), framings_(allocator) {}

    /// Declare the frame's main view. `camera_active` answers `rig` from here on.
    void set_primary_view(cy::camera::RigHandle rig,
                          const cy::camera::RenderViewRequest& request) noexcept;
    /// No main view: `camera_active` answers UNAVAILABLE, as on a dedicated server.
    void clear_primary_view() noexcept { primary_ = cy::camera::RigHandle{}; }

    /// The framing requested for `rig`, or null when none has been.
    [[nodiscard]] const Framing* framing(cy::camera::RigHandle rig) const noexcept;

    [[nodiscard]] CyResult active(CyCamera& out_camera) noexcept override;
    [[nodiscard]] CyResult view(CyCamera camera, CyCameraView& out_view) noexcept override;
    [[nodiscard]] CyResult screen_to_ray(CyCamera camera, f32 screen_x, f32 screen_y,
                                         CyRay& out_ray) noexcept override;
    [[nodiscard]] CyResult world_to_screen(CyCamera camera, Span<const f32> points_xyz,
                                           Span<CyScreenPoint> out_points) noexcept override;
    [[nodiscard]] CyResult set_target(CyCamera camera,
                                      const CyCameraTarget& target) noexcept override;
    [[nodiscard]] CyResult set_pose(CyCamera camera, const CyPose& pose) noexcept override;
    [[nodiscard]] CyResult clear_pose(CyCamera camera) noexcept override;

private:
    struct FramingRecord {
        u64 rig = 0;
        Framing framing;
    };

    /// The live rig `camera` names, or NOT_FOUND.
    [[nodiscard]] CyResult live_rig(CyCamera camera, cy::camera::RigHandle& out) const noexcept;
    /// The evaluated camera of the primary view, with its render view in `out_view`, when `camera`
    /// is it; otherwise null, with the reported failure in `result`.
    [[nodiscard]] const cy::camera::EvaluatedCamera* primary_view(
        CyCamera camera, cy::render::ViewDescription& out_view, CyResult& result) const noexcept;
    [[nodiscard]] CyResult record_framing(u64 rig, const CyCameraTarget& target) noexcept;

    cy::camera::CameraServer* server_;
    cy::camera::RigHandle primary_;
    cy::camera::RenderViewRequest request_;
    Array<FramingRecord> framings_;
};

/// Bind `adapter` as `host`'s camera backend (`host.game.camera`), or unbind with null. The one
/// place an embedder wires the camera service; the adapter must outlive the binding.
void bind(cy::abi::Host& host, CameraAdapter* adapter) noexcept;

}  // namespace cy::game_backend
