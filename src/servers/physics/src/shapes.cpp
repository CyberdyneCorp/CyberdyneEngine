// Shape validation, bounds, mass properties and the cache key. Task 4.2.1.
//
// THE CACHE KEY IS THE INTERESTING PART. `physics` — "Shape sharing": 1 000 entities with an
// identical box collider produce ONE shape. That is a property of the key, not of a backend, so the
// key is computed here and both backends look up with it. It hashes CONTENTS, never pointers: a
// mesh uploaded twice from two buffers is one shape, which is the case the requirement is about.

#include <cy/servers/physics/shapes.h>

#include <cy/core/determinism/hash.h>
#include <cy/core/math/scalar.h>

#include <cstring>

namespace cy::physics {
namespace {

/// `determinism::fold_hash` over the bit pattern of a float.
///
/// The bits rather than the value, because two descriptions are the same shape only if they are
/// bit-identical — 0.5f and 0.5f + 1ulp describe different boxes, and a tolerance here would share
/// a shape between two colliders an author deliberately made different. -0.0f is normalised to 0.0f
/// so that a negated zero, which compares equal, hashes equal.
[[nodiscard]] u64 fold_f32(u64 accumulator, f32 value) noexcept {
    const f32 normalised = value == 0.0f ? 0.0f : value;
    u32 bits = 0;
    std::memcpy(&bits, &normalised, sizeof(bits));
    return determinism::fold_hash(accumulator, static_cast<u64>(bits));
}

[[nodiscard]] u64 fold_vec3(u64 accumulator, Vec3 v) noexcept {
    return fold_f32(fold_f32(fold_f32(accumulator, v.x), v.y), v.z);
}

[[nodiscard]] bool finite(Vec3 v) noexcept {
    return math::is_finite(v.x) && math::is_finite(v.y) && math::is_finite(v.z);
}

}  // namespace

const char* shape_type_name(ShapeType value) noexcept {
    switch (value) {
        case ShapeType::Sphere:
            return "sphere";
        case ShapeType::Box:
            return "box";
        case ShapeType::Capsule:
            return "capsule";
        case ShapeType::Cylinder:
            return "cylinder";
        case ShapeType::ConvexHull:
            return "convex-hull";
        case ShapeType::TriangleMesh:
            return "triangle-mesh";
        case ShapeType::HeightField:
            return "height-field";
        case ShapeType::Compound:
            return "compound";
        case ShapeType::Plane:
            return "plane";
    }
    return "unknown";
}

/// The per-kind halves of `validate()`, one function each.
///
/// Split out because the switch that dispatches them is otherwise one function carrying nine
/// unrelated rule sets, and a reader looking for "what makes a height field invalid" has to walk
/// past eight other shapes to find it. The dispatcher below is then a table.
namespace {

[[nodiscard]] Status validate_sphere(const ShapeDescription& d) noexcept {
    if (d.radius <= 0.0f || !math::is_finite(d.radius)) {
        return fail(ErrorCode::InvalidArgument, "sphere: radius must be positive");
    }
    return ok();
}

[[nodiscard]] Status validate_box(const ShapeDescription& d) noexcept {
    if (!finite(d.half_extents) || d.half_extents.x <= 0.0f || d.half_extents.y <= 0.0f ||
        d.half_extents.z <= 0.0f) {
        return fail(ErrorCode::InvalidArgument, "box: every half extent must be positive");
    }
    return ok();
}

[[nodiscard]] Status validate_round(const ShapeDescription& d) noexcept {
    if (d.radius <= 0.0f || !math::is_finite(d.radius)) {
        return fail(ErrorCode::InvalidArgument, "capsule/cylinder: radius must be positive");
    }
    if (d.half_height < 0.0f || !math::is_finite(d.half_height)) {
        return fail(ErrorCode::InvalidArgument,
                    "capsule/cylinder: half_height must not be negative");
    }
    return ok();
}

[[nodiscard]] Status validate_hull(const ShapeDescription& d) noexcept {
    // Four is a tetrahedron, the smallest hull with a volume. Three points are a triangle and a
    // hull built from one is a degenerate shape whose inertia tensor is singular — which the solver
    // discovers as a NaN several frames later.
    if (d.points == nullptr || d.point_count < 4) {
        return fail(ErrorCode::InvalidArgument, "convex hull: at least four points are required");
    }
    return ok();
}

[[nodiscard]] Status validate_mesh(const ShapeDescription& d) noexcept {
    if (d.vertices == nullptr || d.vertex_count < 3 || d.indices == nullptr || d.index_count < 3) {
        return fail(ErrorCode::InvalidArgument, "triangle mesh: vertices and indices are required");
    }
    if ((d.index_count % 3U) != 0U) {
        return fail(ErrorCode::InvalidArgument,
                    "triangle mesh: index_count is not a multiple of three");
    }
    return ok();
}

[[nodiscard]] Status validate_height_field(const ShapeDescription& d) noexcept {
    if (d.height_field.samples == nullptr || d.height_field.sample_count_x < 2 ||
        d.height_field.sample_count_z < 2) {
        return fail(ErrorCode::InvalidArgument,
                    "height field: at least a 2x2 sample grid is required");
    }
    if (d.height_field.scale.x <= 0.0f || d.height_field.scale.z <= 0.0f) {
        return fail(ErrorCode::InvalidArgument,
                    "height field: the X and Z scales must be positive");
    }
    return ok();
}

[[nodiscard]] Status validate_compound(const ShapeDescription& d) noexcept {
    if (d.children == nullptr || d.child_count == 0) {
        return fail(ErrorCode::InvalidArgument, "compound: at least one child is required");
    }
    for (u32 index = 0; index < d.child_count; ++index) {
        if (d.children[index].shape.is_null()) {
            return fail(ErrorCode::InvalidArgument, "compound: a child shape is null");
        }
    }
    return ok();
}

[[nodiscard]] Status validate_plane(const ShapeDescription& d) noexcept {
    if (!finite(d.plane.normal) || math::nearly_zero(length_squared(d.plane.normal))) {
        return fail(ErrorCode::InvalidArgument, "plane: the normal is degenerate");
    }
    return ok();
}

}  // namespace

Status validate(const ShapeDescription& d) noexcept {
    if (d.density <= 0.0f || !math::is_finite(d.density)) {
        return fail(ErrorCode::InvalidArgument, "shape: density must be positive and finite");
    }
    if (d.convex_radius < 0.0f) {
        return fail(ErrorCode::InvalidArgument, "shape: convex_radius is negative");
    }
    switch (d.type) {
        case ShapeType::Sphere:
            return validate_sphere(d);
        case ShapeType::Box:
            return validate_box(d);
        case ShapeType::Capsule:
        case ShapeType::Cylinder:
            return validate_round(d);
        case ShapeType::ConvexHull:
            return validate_hull(d);
        case ShapeType::TriangleMesh:
            return validate_mesh(d);
        case ShapeType::HeightField:
            return validate_height_field(d);
        case ShapeType::Compound:
            return validate_compound(d);
        case ShapeType::Plane:
            return validate_plane(d);
    }
    return fail(ErrorCode::InvalidArgument, "shape: unknown type");
}

Aabb local_bounds(const ShapeDescription& d) noexcept {
    switch (d.type) {
        case ShapeType::Sphere:
            return Aabb::from_center_extents(Vec3{}, Vec3{d.radius, d.radius, d.radius});
        case ShapeType::Box:
            return Aabb::from_center_extents(Vec3{}, d.half_extents);
        case ShapeType::Capsule: {
            // The cylindrical section runs along local Y, capped by a hemisphere at each end.
            const f32 half = d.half_height + d.radius;
            return Aabb::from_center_extents(Vec3{}, Vec3{d.radius, half, d.radius});
        }
        case ShapeType::Cylinder:
            return Aabb::from_center_extents(Vec3{}, Vec3{d.radius, d.half_height, d.radius});
        case ShapeType::ConvexHull: {
            Aabb box = Aabb::empty();
            for (u32 index = 0; index < d.point_count; ++index) {
                box.grow(d.points[index]);
            }
            return box;
        }
        case ShapeType::TriangleMesh: {
            Aabb box = Aabb::empty();
            for (u32 index = 0; index < d.vertex_count; ++index) {
                box.grow(d.vertices[index]);
            }
            return box;
        }
        case ShapeType::HeightField: {
            const HeightFieldDescription& hf = d.height_field;
            Aabb box = Aabb::empty();
            const u32 count = hf.sample_count_x * hf.sample_count_z;
            for (u32 index = 0; index < count; ++index) {
                const f32 sample = hf.samples[index];
                if (sample == kHeightFieldHole) {
                    continue;  // a hole contributes no geometry, and FLT_MAX would swallow the box
                }
                const u32 x = index % hf.sample_count_x;
                const u32 z = index / hf.sample_count_x;
                box.grow(hf.offset + Vec3{static_cast<f32>(x) * hf.scale.x, sample * hf.scale.y,
                                          static_cast<f32>(z) * hf.scale.z});
            }
            return box;
        }
        case ShapeType::Compound: {
            Aabb box = Aabb::empty();
            for (u32 index = 0; index < d.child_count; ++index) {
                // A child's own bounds are the backend's — the handle is opaque here — so a
                // compound's local bounds are the bound of its children's ORIGINS. The backend
                // computes the real thing; this is what the cache key and a diagnostic need.
                box.grow(d.children[index].local.translation);
            }
            return box;
        }
        case ShapeType::Plane:
            // A half-space is unbounded. Returning the empty box rather than an infinite one is
            // deliberate: an infinite box propagates into every merge it takes part in, and every
            // caller that unions bounds would then have an infinite world.
            return Aabb::empty();
    }
    return Aabb::empty();
}

f32 volume(const ShapeDescription& d) noexcept {
    switch (d.type) {
        case ShapeType::Sphere:
            return (4.0f / 3.0f) * math::kPi * d.radius * d.radius * d.radius;
        case ShapeType::Box:
            return 8.0f * d.half_extents.x * d.half_extents.y * d.half_extents.z;
        case ShapeType::Capsule:
            return (math::kPi * d.radius * d.radius * 2.0f * d.half_height) +
                   ((4.0f / 3.0f) * math::kPi * d.radius * d.radius * d.radius);
        case ShapeType::Cylinder:
            return math::kPi * d.radius * d.radius * 2.0f * d.half_height;
        case ShapeType::ConvexHull:
            // Half the bounding box, which is the mean ratio of a hull to its bounds. Approximate
            // and said so in the header: an exact hull volume needs the hull, and the hull is the
            // backend's. A body that needs an exact mass states one.
            return local_bounds(d).volume() * 0.5f;
        case ShapeType::TriangleMesh:
        case ShapeType::HeightField:
        case ShapeType::Plane:
        // A compound's volume is its children's, and only the backend can resolve their handles.
        // Folded in with the shapes that genuinely have no volume because the ANSWER is the same
        // number for a different reason, and two branches returning the same literal read to a
        // linter — correctly — as one branch written twice.
        case ShapeType::Compound:
            return 0.0f;
    }
    return 0.0f;
}

Vec3 unit_inertia(const ShapeDescription& d) noexcept {
    switch (d.type) {
        case ShapeType::Sphere: {
            const f32 i = 0.4f * d.radius * d.radius;
            return Vec3{i, i, i};
        }
        case ShapeType::Box: {
            const Vec3 e = d.half_extents * 2.0f;
            return Vec3{((e.y * e.y) + (e.z * e.z)) / 12.0f, ((e.x * e.x) + (e.z * e.z)) / 12.0f,
                        ((e.x * e.x) + (e.y * e.y)) / 12.0f};
        }
        case ShapeType::Cylinder:
        case ShapeType::Capsule: {
            // The cylinder's tensor, which is what a capsule is within a few percent and is what a
            // gameplay body wants. The caps' contribution is second-order for the aspect ratios a
            // character or a barrel uses.
            const f32 h = d.half_height * 2.0f;
            const f32 radial = ((3.0f * d.radius * d.radius) + (h * h)) / 12.0f;
            return Vec3{radial, 0.5f * d.radius * d.radius, radial};
        }
        case ShapeType::ConvexHull:
        case ShapeType::Compound: {
            const Aabb box = local_bounds(d);
            if (box.is_empty()) {
                return Vec3{1.0f, 1.0f, 1.0f};
            }
            const Vec3 e = box.size();
            return Vec3{((e.y * e.y) + (e.z * e.z)) / 12.0f, ((e.x * e.x) + (e.z * e.z)) / 12.0f,
                        ((e.x * e.x) + (e.y * e.y)) / 12.0f};
        }
        case ShapeType::TriangleMesh:
        case ShapeType::HeightField:
        case ShapeType::Plane:
            // Static only, so the tensor is never inverted. Identity rather than zero, because a
            // zero tensor inverts to an infinity if anybody ever does.
            return Vec3{1.0f, 1.0f, 1.0f};
    }
    return Vec3{1.0f, 1.0f, 1.0f};
}

u64 shape_key(const ShapeDescription& d) noexcept {
    u64 key = determinism::fold_hash(0, static_cast<u64>(d.type));
    key = fold_f32(key, d.density);
    key = fold_f32(key, d.convex_radius);
    switch (d.type) {
        case ShapeType::Sphere:
            key = fold_f32(key, d.radius);
            break;
        case ShapeType::Box:
            key = fold_vec3(key, d.half_extents);
            break;
        case ShapeType::Capsule:
        case ShapeType::Cylinder:
            key = fold_f32(fold_f32(key, d.radius), d.half_height);
            break;
        case ShapeType::ConvexHull:
            key = determinism::fold_hash(key, d.point_count);
            for (u32 index = 0; index < d.point_count; ++index) {
                key = fold_vec3(key, d.points[index]);
            }
            break;
        case ShapeType::TriangleMesh:
            key = determinism::fold_hash(key, d.vertex_count);
            key = determinism::fold_hash(key, d.index_count);
            for (u32 index = 0; index < d.vertex_count; ++index) {
                key = fold_vec3(key, d.vertices[index]);
            }
            for (u32 index = 0; index < d.index_count; ++index) {
                key = determinism::fold_hash(key, d.indices[index]);
            }
            break;
        case ShapeType::HeightField: {
            const HeightFieldDescription& hf = d.height_field;
            key = determinism::fold_hash(key, hf.sample_count_x);
            key = determinism::fold_hash(key, hf.sample_count_z);
            key = fold_vec3(fold_vec3(key, hf.offset), hf.scale);
            const u32 count = hf.sample_count_x * hf.sample_count_z;
            for (u32 index = 0; index < count; ++index) {
                key = fold_f32(key, hf.samples[index]);
            }
            break;
        }
        case ShapeType::Compound:
            key = determinism::fold_hash(key, d.child_count);
            for (u32 index = 0; index < d.child_count; ++index) {
                const CompoundChild& child = d.children[index];
                key = determinism::fold_hash(key, child.shape.bits());
                key = fold_vec3(key, child.local.translation);
                key = fold_f32(fold_f32(fold_f32(fold_f32(key, child.local.rotation.x),
                                                 child.local.rotation.y),
                                        child.local.rotation.z),
                               child.local.rotation.w);
                key = fold_vec3(key, child.local.scale);
            }
            break;
        case ShapeType::Plane:
            key = fold_f32(fold_vec3(key, d.plane.normal), d.plane.d);
            break;
    }
    return key;
}

}  // namespace cy::physics
