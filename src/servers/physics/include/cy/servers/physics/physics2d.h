#pragma once
// The 2D API: genuinely two-dimensional, over a 3D solver constrained to a plane. Task 4.2.1.
//
// `physics` — "2D physics": 2D "SHALL be provided by running the 3D backend constrained to a
// plane: bodies are created with locked Z translation and locked X/Y rotation, and 2D shapes are
// extruded to thin 3D shapes. This SHALL be an implementation strategy, not a leaked detail: the 2D
// API, components, and queries SHALL be genuinely 2D (`Vec2`, angles, 2D shapes)."
//
// So: nothing below returns a `Vec3`, takes a `Vec3`, or has a parameter named `z`. The scenario is
// "gameplay code queries a 2D raycast, passes a `Vec2` origin and direction and receives a `Vec2`
// normal, with no awareness of the third axis", and the way to satisfy it is for the third axis to
// be unspellable at this API rather than defaulted.
//
// THE PLANE IS Z = 0 AND THE FACTS ABOUT IT ARE HERE, ONCE. Every conversion in this file goes
// through `to_3d`/`to_2d` and `angle_to_quat`/`quat_to_angle`, so there is one place that knows the
// convention: X and Y are the plane, +Z points at the viewer, and a positive angle turns
// counter-clockwise about +Z. A second conversion written at a call site is how a project ends up
// with sprites that face backwards on Tuesdays.
//
// AN INDEPENDENT 2D SOLVER IS DEFERRED, which `physics` says outright — "to be revisited if
// profiling shows the constrained-3D approach is inadequate for 2D-heavy projects". This file is
// what makes revisiting it cheap: gameplay code never learned that there was a third axis, so
// replacing what is underneath does not change a call site.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/vec.h>
#include <cy/servers/physics/server.h>

namespace cy::physics::two_d {

/// The thickness a 2D shape is extruded to, in metres. Thick enough that a fast body cannot pass
/// through the slab between two steps at any speed a 2D game uses; thin enough that it never
/// becomes visible in a query.
inline constexpr f32 kExtrusionHalfDepth = 0.5f;

[[nodiscard]] constexpr Vec3 to_3d(Vec2 v) noexcept {
    return Vec3{v.x, v.y, 0.0f};
}
[[nodiscard]] constexpr Vec2 to_2d(Vec3 v) noexcept {
    return Vec2{v.x, v.y};
}

/// A rotation about +Z, in radians.
[[nodiscard]] Quat angle_to_quat(f32 radians) noexcept;
[[nodiscard]] f32 quat_to_angle(const Quat& rotation) noexcept;

/// A placement in the plane.
struct Transform2D {
    Vec2 position{0.0f, 0.0f};
    f32 rotation = 0.0f;
    Vec2 scale{1.0f, 1.0f};
};

[[nodiscard]] Transform to_3d(const Transform2D& t) noexcept;
[[nodiscard]] Transform2D to_2d(const Transform& t) noexcept;

/// The 2D shapes, each extruded to a slab `2 * kExtrusionHalfDepth` deep.
enum class ShapeType2D : u8 { Circle = 0, Box, Capsule, Polygon };

struct ShapeDescription2D {
    ShapeType2D type = ShapeType2D::Circle;
    f32 radius = 0.5f;
    /// Capsule: half the length of the straight section.
    f32 half_length = 0.5f;
    /// Box: half width and half height.
    Vec2 half_extents{0.5f, 0.5f};
    /// Polygon: convex, counter-clockwise. Not owned.
    const Vec2* points = nullptr;
    u32 point_count = 0;
    f32 density = 1000.0f;
};

/// Extrude to the 3D description the server takes. `scratch` receives the extruded hull points for
/// a polygon and must outlive the `create_shape` call — a polygon becomes twice as many 3D points,
/// and allocating them inside a conversion helper would put an allocation on the shape-creation
/// path for the one shape kind that already has one.
[[nodiscard]] Expected<ShapeDescription, Error> extrude(const ShapeDescription2D& shape,
                                                        Span<Vec3> scratch) noexcept;

struct BodyDescription2D {
    Name name;
    MotionType motion = MotionType::Dynamic;
    Transform2D transform;
    Vec2 linear_velocity{0.0f, 0.0f};
    /// Radians per second about +Z.
    f32 angular_velocity = 0.0f;
    f32 mass = 0.0f;
    f32 linear_damping = 0.05f;
    f32 angular_damping = 0.05f;
    f32 gravity_scale = 1.0f;
    bool allow_sleeping = true;
    const ColliderDescription* colliders = nullptr;
    u32 collider_count = 0;
    UserData user_data = 0;
};

/// Create a body locked to the plane. This is the whole of the strategy: `kLockPlaneXY`, applied
/// here so a caller cannot forget it.
[[nodiscard]] Expected<BodyHandle, Error> create_body(
    PhysicsServer& server, WorldHandle world, const BodyDescription2D& description) noexcept;

struct BodyState2D {
    Transform2D transform;
    Vec2 linear_velocity{0.0f, 0.0f};
    f32 angular_velocity = 0.0f;
    bool asleep = false;
};

[[nodiscard]] Expected<BodyState2D, Error> body_state(const PhysicsServer& server,
                                                      BodyHandle body) noexcept;

struct RayCastInput2D {
    Vec2 origin{0.0f, 0.0f};
    Vec2 direction{1.0f, 0.0f};
    f32 max_distance = 1000.0f;
};

struct RayCastHit2D {
    BodyHandle body;
    UserData user_data = 0;
    Vec2 position{0.0f, 0.0f};
    Vec2 normal{0.0f, 1.0f};
    f32 distance = 0.0f;
    MaterialHandle material;
    bool trigger = false;
};

/// `physics`' "2D API is 2D" scenario, in one signature: a `Vec2` in, a `Vec2` normal out.
[[nodiscard]] Expected<RayCastHit2D, Error> raycast(const PhysicsServer& server, WorldHandle world,
                                                    const RayCastInput2D& input,
                                                    const QueryFilter& filter) noexcept;

}  // namespace cy::physics::two_d
