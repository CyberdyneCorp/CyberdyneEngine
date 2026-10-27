// SPDX-License-Identifier: MIT
// cy/game_backend/camera_collision.h — the camera's collision and occlusion queries, answered by a
// physics world.
//
// `camera-system` requires a rig's collision and occlusion queries to be "batched through the
// physics interface" rather than cast from the rig node: a `Collision` node APPENDS a
// `CameraQuery` to the camera server's batch and applies the answer it is handed on the next
// evaluation (cy/servers/camera/rig.h). The camera server is at layer 2 and cannot name a physics
// world. This is the half above both that answers the batch:
//
//   collision    a sphere of the query's radius swept from the anchor (the target) toward where the
//                rig wants the camera. The fraction is how far it got: a wall, a pillar, or the
//                underside of a stair between them all stop it. A sweep that starts inside
//                something answers zero.
//   occlusion    a ray from where the rig wants the camera back to the anchor: blocked or not.
//
// Bodies the camera must never collide with — the character it frames, whose capsule surrounds
// the anchor — are named with `set_ignored`, and the queries filter by `set_filter`'s layers.
//
// One call a frame, after the frame's rig evaluations and outside the physics step:
//
//     camera.begin_frame();
//     camera.evaluate(rig, context);          // publishes this frame's queries
//     resolver.resolve(camera);               // answers them for the next evaluation
//
// Not thread-safe: the game thread only.

#pragma once

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/camera/server.h>
#include <cy/servers/physics/server.h>

namespace cy::game_backend {

/// Answers a `cy::camera::CameraServer`'s batched queries against one physics world.
class CameraCollisionResolver {
public:
    /// What the last `resolve` did.
    struct Stats {
        u32 collision_queries = 0;
        u32 occlusion_queries = 0;
        /// Collision sweeps that stopped short of the camera, and occlusion rays that were blocked.
        u32 collisions = 0;
        u32 occlusions = 0;
        /// The smallest collision fraction answered; one when nothing was in the way.
        f32 nearest_fraction = 1.0F;
    };

    /// Queries go to `world` on `server`; both are borrowed and outlive the resolver, which
    /// destroys the probe shapes it made in its destructor.
    CameraCollisionResolver(Allocator& allocator, physics::PhysicsServer& server,
                            physics::WorldHandle world) noexcept;
    ~CameraCollisionResolver();

    CameraCollisionResolver(const CameraCollisionResolver&) = delete;
    CameraCollisionResolver& operator=(const CameraCollisionResolver&) = delete;
    CameraCollisionResolver(CameraCollisionResolver&&) = delete;
    CameraCollisionResolver& operator=(CameraCollisionResolver&&) = delete;

    /// The layers a camera probe is and collides with. Defaults to everything.
    void set_filter(const physics::CollisionFilter& filter) noexcept { filter_ = filter; }
    /// Bodies the probe passes through: the framed character's own capsule. Copied.
    [[nodiscard]] Status set_ignored(Span<const physics::BodyHandle> bodies) noexcept;

    /// Answer every query `camera` published since its `begin_frame`, and hand the answers back
    /// through `submit_query_results` for the next evaluation. Fails, and submits nothing, when the
    /// physics world refuses a query — during its step, or for a world that no longer exists.
    [[nodiscard]] Status resolve(camera::CameraServer& camera) noexcept;

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    struct Probe {
        f32 radius = 0.0F;
        physics::ShapeHandle shape;
    };

    /// The sphere of `radius`, made the first time it is asked for.
    [[nodiscard]] Expected<physics::ShapeHandle, Error> probe(f32 radius) noexcept;
    [[nodiscard]] Expected<camera::CameraQueryResult, Error> collide(
        const camera::CameraQuery& query) noexcept;
    [[nodiscard]] Expected<camera::CameraQueryResult, Error> occlude(
        const camera::CameraQuery& query) const noexcept;
    [[nodiscard]] physics::QueryFilter query_filter() const noexcept;

    physics::PhysicsServer* server_;
    physics::WorldHandle world_;
    physics::CollisionFilter filter_;
    Array<physics::BodyHandle> ignored_;
    Array<Probe> probes_;
    Array<camera::CameraQueryResult> results_;
    Stats stats_;
};

}  // namespace cy::game_backend
