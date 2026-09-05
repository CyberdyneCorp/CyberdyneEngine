#pragma once
// Scene queries: raycast, shape cast, overlap and closest point. Task 4.2.4.
//
// `physics` — "Queries": the four kinds, and "every query SHALL accept: layer and mask filters, an
// ignore list, a maximum distance or hit count, a face-culling option, and a flag for whether
// triggers are included". That whole list is `QueryFilter`, once, rather than five arguments
// repeated across four call shapes — which is also what makes "a character raycasts downward and
// excludes its own collider" one line at the call site instead of a post-filter loop that forgets
// the trigger case.
//
// THREADING. `physics` — "Parallel queries": queries "SHALL be thread-safe and SHALL NOT mutate
// simulation state", and "Query during the step": a query attempted mid-step "SHALL be rejected in
// development builds with a diagnostic". Both are properties of `PhysicsServer`'s query methods
// being `const` and of the server's stepping flag, not of anything in this file — see server.h.

#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/core/math/vec.h>
#include <cy/servers/physics/handles.h>
#include <cy/servers/physics/types.h>

namespace cy::physics {

/// Everything a query filters by. One struct for all four query kinds.
struct QueryFilter {
    /// The querying "collider": its layer and the layers it will hit. Filtered mutually against
    /// each candidate, and through the world's collision matrix, exactly as a contact is — so a
    /// raycast cannot hit something the same two bodies would not collide with.
    CollisionFilter filter;

    /// Bodies to skip. Not owned; read during the call. `physics`' "Raycast excluding self".
    const BodyHandle* ignore = nullptr;
    u32 ignore_count = 0;

    /// Sensors are excluded by default: the common query asks about solid geometry, and a query
    /// that silently hit trigger volumes would make a character stand on its own checkpoint.
    bool include_triggers = false;

    /// Skip a triangle whose winding faces away from the ray. Meaningless for the primitives, which
    /// have no back faces, and load-bearing for a triangle mesh.
    bool cull_back_faces = true;

    /// Skip static / kinematic / dynamic bodies. All three on by default.
    bool include_static = true;
    bool include_kinematic = true;
    bool include_dynamic = true;

    [[nodiscard]] constexpr bool includes(MotionType motion) const noexcept {
        switch (motion) {
            case MotionType::Static:
                return include_static;
            case MotionType::Kinematic:
                return include_kinematic;
            case MotionType::Dynamic:
                return include_dynamic;
        }
        return false;
    }

    /// True when `body` is on the ignore list.
    [[nodiscard]] bool ignores(BodyHandle body) const noexcept;
};

// --- Rays --------------------------------------------------------------------------------------

struct RayCastInput {
    Vec3 origin{0.0f, 0.0f, 0.0f};
    /// Expected unit length. Distances are then metres, which is what every consumer assumes.
    Vec3 direction{0.0f, -1.0f, 0.0f};
    f32 max_distance = 1000.0f;
};

/// `physics` — "Raycast excluding self": the hit reports "position, normal, distance, entity, and
/// physics material". `user_data` is the entity — see handles.h for why the server cannot hold one.
struct RayCastHit {
    BodyHandle body;
    UserData user_data = 0;
    Vec3 position{0.0f, 0.0f, 0.0f};
    /// Unit, pointing out of the surface that was hit.
    Vec3 normal{0.0f, 1.0f, 0.0f};
    f32 distance = 0.0f;
    MaterialHandle material;
    bool trigger = false;
};

// --- Shape casts (sweeps) ------------------------------------------------------------------------

struct ShapeCastInput {
    ShapeHandle shape;
    /// Where the shape starts. Its rotation is kept for the whole sweep — a sweep that rotates is
    /// not a sweep, and a backend that pretended otherwise would return a fraction that means
    /// nothing.
    Transform start;
    Vec3 direction{0.0f, -1.0f, 0.0f};
    f32 max_distance = 1.0f;
};

struct ShapeCastHit {
    BodyHandle body;
    UserData user_data = 0;
    /// The point on the hit surface, in world space.
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    /// Metres travelled before the touch.
    f32 distance = 0.0f;
    /// `distance / max_distance`. Kept beside the distance because the collide-and-slide loop wants
    /// the fraction and a footstep probe wants the metres, and deriving one at every call site is
    /// where a division by a zero-length sweep gets written.
    f32 fraction = 0.0f;
    MaterialHandle material;
    bool trigger = false;
    /// True when the shape already overlapped at `start`. The sweep then reports distance 0 and a
    /// normal that separates, which is what a controller needs to depenetrate rather than a hit it
    /// should slide along.
    bool started_penetrating = false;
};

// --- Overlap and closest point
// --------------------------------------------------------------------

struct OverlapInput {
    ShapeHandle shape;
    Transform transform;
};

struct OverlapHit {
    BodyHandle body;
    UserData user_data = 0;
    bool trigger = false;
};

struct ClosestPointInput {
    Vec3 point{0.0f, 0.0f, 0.0f};
    f32 max_distance = 1000.0f;
};

/// The guard `physics`' "Query during the step" scenario requires: "WHEN a query is attempted while
/// the physics step is in progress THEN it SHALL be rejected in development builds with a
/// diagnostic, since the world is mid-solve".
///
/// A FREE FUNCTION IN THE INTERFACE MODULE, NOT A LINE IN EACH BACKEND, for two reasons. It is the
/// same rule for every backend, and a second copy would eventually disagree about the error code or
/// about which builds it applies to. And it is directly testable: a case can assert both halves —
/// that it rejects while stepping and that it is compiled out where `CY_ASSERT` is — without
/// contriving a way to be inside somebody's solver.
///
/// In Profile and Shipping it always succeeds: the check is a branch on the hot path, and the
/// situation it detects is a programmer error that development already caught.
[[nodiscard]] Status reject_query_during_step(bool stepping) noexcept;

struct ClosestPoint {
    BodyHandle body;
    UserData user_data = 0;
    /// The point on the body's surface nearest the query point.
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    /// Unsigned metres. Zero when the query point is inside the body.
    f32 distance = 0.0f;
    MaterialHandle material;
};

}  // namespace cy::physics
