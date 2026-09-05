#pragma once
// Collision shapes, their cache key, and the mass properties derived from them. Task 4.2.1.
//
// `physics` — "Physics components": "Shapes SHALL include: sphere, box, capsule, cylinder, convex
// hull, triangle mesh (static only), height field, and compound." All eight are here, plus a plane
// — a half-space, which is not in that list but is what a test floor is, and what a backend without
// one has to fake with a very large box that then dominates the broad phase.
//
// ONE DESCRIPTION STRUCT, NOT A CLASS HIERARCHY. A shape crosses the ABI at M4 and a scene file at
// M2; both want a POD. The unused members of a sphere's description cost eleven words and buy a
// type that is trivially copyable, trivially hashable, and describable by the reflection generator
// without a virtual anything. `validate()` is what makes the unused members honest: a sphere whose
// `half_extents` were filled in is not silently a box.
//
// THE CACHE KEY IS COMPUTED HERE, NOT IN A BACKEND. `physics` — "Shape sharing": 1 000 entities
// with an identical box collider SHALL produce one shape. If each backend hashed its own way, the
// property would be true of whichever backend somebody measured. `shape_key()` is the one hash, and
// `cy_physics`'s conformance suite asserts it over both backends.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/array.h>
#include <cy/servers/physics/handles.h>

namespace cy::physics {

enum class ShapeType : u8 {
    Sphere = 0,
    Box,
    Capsule,
    Cylinder,
    ConvexHull,
    /// Static bodies only. `physics` — "Triangle mesh on a dynamic body": placing one on a dynamic
    /// body "SHALL be rejected with a diagnostic recommending convex decomposition".
    TriangleMesh,
    HeightField,
    Compound,
    /// A half-space: everything below the plane is solid. Static bodies only, for the same reason
    /// a triangle mesh is: it has no finite volume and therefore no inertia tensor.
    Plane,
};

const char* shape_type_name(ShapeType value) noexcept;

/// True for the shapes that may only carry a static body.
[[nodiscard]] constexpr bool is_static_only(ShapeType type) noexcept {
    return type == ShapeType::TriangleMesh || type == ShapeType::HeightField ||
           type == ShapeType::Plane;
}

/// A height sample that is a hole rather than ground.
///
/// `physics` — "Heightfield holes SHALL be representable, so a cave entrance is not blocked by an
/// invisible floor." A sentinel rather than a parallel bitmask: the samples are already the
/// authoritative array, a second array is a second thing to keep in step, and this is the value
/// Jolt's own height field uses for the same purpose, so the mapping is an identity.
inline constexpr f32 kHeightFieldHole = 3.402823466e+38F;  // FLT_MAX

struct HeightFieldDescription {
    /// Row-major, `sample_count_x * sample_count_z` samples, in the shape's local space. Not owned:
    /// `create_shape` copies what it needs before returning.
    const f32* samples = nullptr;
    u32 sample_count_x = 0;
    u32 sample_count_z = 0;
    /// The world-space corner the first sample sits at, and the spacing between samples in X and Z.
    /// `scale.y` multiplies the sample values.
    Vec3 offset{0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

struct CompoundChild {
    ShapeHandle shape;
    Transform local;
};

/// One shape, described. Only the members its `type` names are read; `validate()` rejects the rest.
struct ShapeDescription {
    ShapeType type = ShapeType::Sphere;

    /// Sphere, Capsule, Cylinder.
    f32 radius = 0.5f;
    /// Capsule and Cylinder: half the height of the cylindrical section, so a capsule's total
    /// height is `2 * (half_height + radius)`. Stated because the other convention — half the total
    /// height — differs by exactly one radius and produces a character that sinks into the floor.
    f32 half_height = 0.5f;
    /// Box.
    Vec3 half_extents{0.5f, 0.5f, 0.5f};
    /// Plane. The solid side is the negative side: `signed_distance(p) < 0` is inside.
    Plane plane;

    /// ConvexHull. Not owned; copied by `create_shape`.
    const Vec3* points = nullptr;
    u32 point_count = 0;

    /// TriangleMesh. Not owned; copied by `create_shape`. `index_count` is a multiple of three.
    const Vec3* vertices = nullptr;
    u32 vertex_count = 0;
    const u32* indices = nullptr;
    u32 index_count = 0;

    HeightFieldDescription height_field;

    /// Compound. Not owned; the children's handles are retained by `create_shape`.
    const CompoundChild* children = nullptr;
    u32 child_count = 0;

    /// Kilograms per cubic metre, used when a body derives its mass from its colliders. Overridden
    /// by a collider's material when one is set.
    f32 density = 1000.0f;

    /// How far a convex shape's surface is pushed out before the solver sees it. Zero is legal and
    /// is what a query shape wants.
    f32 convex_radius = 0.0f;
};

/// Reject a description a backend cannot build, naming the member that is wrong.
[[nodiscard]] Status validate(const ShapeDescription& description) noexcept;

/// The bounds of the shape in its own space. Exact for the primitives; for a mesh, a hull and a
/// height field it is the bound of the data.
[[nodiscard]] Aabb local_bounds(const ShapeDescription& description) noexcept;

/// The shape's volume in cubic metres, which is what turns a density into a mass.
///
/// Zero for the shapes that have no finite volume — plane, height field, triangle mesh — which is
/// why those may only carry a static body. A convex hull is approximated by its bounding box's
/// volume scaled by 1/2, an approximation stated here rather than hidden: an exact hull volume
/// needs the hull, and the hull is the backend's.
[[nodiscard]] f32 volume(const ShapeDescription& description) noexcept;

/// The diagonal inertia tensor of a unit-mass shape about its centre, for the primitives.
///
/// Returned as the diagonal because every primitive here is inertially aligned with its own axes.
/// A backend that computes a full tensor is free to; this is what the reference backend uses and
/// what a test compares a backend against.
[[nodiscard]] Vec3 unit_inertia(const ShapeDescription& description) noexcept;

/// The cache key. Two descriptions that describe the same shape hash equal; two that do not, do
/// not.
///
/// Array-backed shapes hash their contents, not their pointers — a mesh uploaded twice from two
/// buffers is one shape, which is the whole point of the cache. That makes the key O(n) in the
/// data, which is why `create_shape` computes it once per call and not per body.
[[nodiscard]] u64 shape_key(const ShapeDescription& description) noexcept;

}  // namespace cy::physics
