// Engine shape descriptions to Jolt shapes. Task 4.2.2.

#include "jolt_shapes.h"

// clang-format off
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
// clang-format on

namespace cy::physics::jolt {
namespace {

[[nodiscard]] Expected<JPH::Ref<JPH::Shape>, Error> finish(
    const JPH::ShapeSettings& settings) noexcept {
    const JPH::Shape::ShapeResult result = settings.Create();
    if (result.HasError()) {
        // Jolt's message is a `JPH::String` owned by the result, which does not outlive this scope,
        // so it cannot be carried in `Error::message` — that field is a literal or storage the
        // caller owns. The shape kind is in the caller's diagnostic; this says which layer refused.
        return fail(ErrorCode::InvalidArgument, "jolt: the shape settings were rejected");
    }
    return result.Get();
}

}  // namespace

Expected<JPH::Ref<JPH::Shape>, Error> build_shape(const ShapeDescription& description,
                                                  ChildResolver resolve, void* context) noexcept {
    switch (description.type) {
        case ShapeType::Sphere: {
            const JPH::SphereShapeSettings settings(description.radius);
            return finish(settings);
        }
        case ShapeType::Box: {
            // The convex radius must not exceed the smallest half extent, or Jolt refuses the
            // shape. Clamped here rather than reported, because the engine's default of zero is
            // always legal and a caller that asked for a rounded box wants the largest rounding
            // that fits.
            const f32 smallest =
                math::min(description.half_extents.x,
                          math::min(description.half_extents.y, description.half_extents.z));
            const f32 radius = math::clamp(description.convex_radius, 0.0f, smallest * 0.5f);
            const JPH::BoxShapeSettings settings(to_jolt(description.half_extents), radius);
            return finish(settings);
        }
        case ShapeType::Capsule: {
            // Jolt's `inHalfHeightOfCylinder` is the engine's `half_height` exactly — both are the
            // straight section, excluding the caps. Stated because the other reading differs by one
            // radius and produces a character standing inside the floor.
            const JPH::CapsuleShapeSettings settings(description.half_height, description.radius);
            return finish(settings);
        }
        case ShapeType::Cylinder: {
            const f32 radius =
                math::clamp(description.convex_radius, 0.0f,
                            math::min(description.radius, description.half_height) * 0.5f);
            const JPH::CylinderShapeSettings settings(description.half_height, description.radius,
                                                      radius);
            return finish(settings);
        }
        case ShapeType::ConvexHull: {
            JPH::Array<JPH::Vec3> points;
            points.reserve(description.point_count);
            for (u32 index = 0; index < description.point_count; ++index) {
                points.push_back(to_jolt(description.points[index]));
            }
            const JPH::ConvexHullShapeSettings settings(points);
            return finish(settings);
        }
        case ShapeType::TriangleMesh: {
            JPH::VertexList vertices;
            vertices.reserve(description.vertex_count);
            for (u32 index = 0; index < description.vertex_count; ++index) {
                vertices.push_back(JPH::Float3(description.vertices[index].x,
                                               description.vertices[index].y,
                                               description.vertices[index].z));
            }
            JPH::IndexedTriangleList triangles;
            triangles.reserve(description.index_count / 3U);
            for (u32 index = 0; index + 2 < description.index_count; index += 3) {
                triangles.push_back(JPH::IndexedTriangle(description.indices[index],
                                                         description.indices[index + 1],
                                                         description.indices[index + 2]));
            }
            const JPH::MeshShapeSettings settings(vertices, triangles);
            return finish(settings);
        }
        case ShapeType::HeightField: {
            const HeightFieldDescription& field = description.height_field;
            // Jolt's height field is square and its side is a multiple of its block size. The
            // engine's is rectangular, so the mismatch is REPORTED rather than papered over by
            // padding: a padded field would silently add collision where the caller put none, which
            // is exactly the "invisible floor" the hole support exists to avoid.
            if (field.sample_count_x != field.sample_count_z) {
                return fail(ErrorCode::Unsupported,
                            "jolt: a height field must be square; pad the samples to a square grid "
                            "with kHeightFieldHole in the cells that carry no collision");
            }
            // The engine's hole sentinel is FLT_MAX and so is Jolt's `cNoCollisionValue`, which is
            // why shapes.h chose that value: the mapping is an identity and no sample has to be
            // rewritten on the way in.
            static_assert(kHeightFieldHole == JPH::HeightFieldShapeConstants::cNoCollisionValue,
                          "the engine's height-field hole sentinel must be Jolt's, or every sample "
                          "would have to be translated on creation and on every regional update");
            const JPH::HeightFieldShapeSettings settings(
                field.samples, to_jolt(field.offset), to_jolt(field.scale), field.sample_count_x);
            return finish(settings);
        }
        case ShapeType::Compound: {
            JPH::StaticCompoundShapeSettings settings;
            for (u32 index = 0; index < description.child_count; ++index) {
                const CompoundChild& child = description.children[index];
                const JPH::Shape* shape = resolve(context, child.shape);
                if (shape == nullptr) {
                    return fail(ErrorCode::NotFound,
                                "jolt: a compound child names a shape that is not live");
                }
                settings.AddShape(to_jolt(child.local.translation), to_jolt(child.local.rotation),
                                  shape);
            }
            return finish(settings);
        }
        case ShapeType::Plane: {
            // Jolt's plane is `n . x + c = 0` with the solid side NEGATIVE, which is the engine's
            // convention too (shapes.h). The two constants are therefore the same number and no
            // sign flip belongs here — a flip written "to be safe" would put the world's floor
            // above the world.
            const JPH::PlaneShapeSettings settings(
                JPH::Plane(to_jolt(description.plane.normal), description.plane.d));
            return finish(settings);
        }
    }
    return fail(ErrorCode::InvalidArgument, "jolt: unknown shape type");
}

}  // namespace cy::physics::jolt
