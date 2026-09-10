// The GPU pose world and the handoff to skinning: current and previous matrices, add and remove
// without a rebuild, the upload range, and a `SkinningDescriptor` built from a real offset — the
// refusal M6 wrote, answered. M8.b tasks 5.3 and 5.4.

#include <cy/animation/evaluate.h>
#include <cy/animation/pose_world.h>
#include <cy/servers/render/geometry/skinning.h>
#include <cy/test/test.h>

#include "fixture.h"

using namespace cy;
using namespace cy::animation;
using namespace cy::animation::testing;

namespace {

[[nodiscard]] Status publish_identity(PoseWorld& world, PoseHandle handle, u32 bones, f32 height) {
    Array<Mat4> matrices(allocator());
    if (Status sized = matrices.resize(bones); !sized) {
        return sized;
    }
    for (u32 bone = 0; bone < bones; ++bone) {
        matrices[bone] = Mat4::from_translation(Vec3{0.0F, height, 0.0F});
    }
    return world.publish(handle, matrices.span());
}

}  // namespace

CY_TEST_CASE("pose world: instances are added and removed without rebuilding the world") {
    PoseWorld world(allocator());
    Expected<PoseHandle, Error> first = world.add(12);
    Expected<PoseHandle, Error> second = world.add(30);
    Expected<PoseHandle, Error> third = world.add(12);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_REQUIRE(third.has_value());
    CY_CHECK_EQ(world.stats().instances, 3U);
    // Two frames of each: 24, 60 and 24 matrices.
    CY_CHECK_EQ(world.stats().live_matrices, 108U);

    const u32 second_base = world.base_offset(*second);
    const u32 third_base = world.base_offset(*third);

    CY_REQUIRE(world.remove(*first).has_value());
    // Nothing else moved. That is the whole of "without rebuilding the world": a streaming world
    // removes instances constantly and every other handle stays valid.
    CY_CHECK_EQ(world.base_offset(*second), second_base);
    CY_CHECK_EQ(world.base_offset(*third), third_base);
    CY_CHECK_FALSE(world.live(*first));

    // The freed range is reused by the next instance of the same size rather than growing the
    // array.
    const u32 capacity_before = world.stats().capacity_matrices;
    Expected<PoseHandle, Error> fourth = world.add(12);
    CY_REQUIRE(fourth.has_value());
    CY_CHECK_EQ(world.stats().capacity_matrices, capacity_before);
    CY_CHECK_EQ(world.stats().reused_slots, 1U);

    // The stale handle is refused rather than reading whatever moved in.
    CY_CHECK_FALSE(world.live(*first));
    CY_CHECK_FALSE(world.publish(*first, Span<const Mat4>{}).has_value());
    CY_CHECK_FALSE(world.remove(*first).has_value());
}

CY_TEST_CASE("pose world: publishing rotates current and previous without copying either") {
    PoseWorld world(allocator());
    Expected<PoseHandle, Error> handle = world.add(4);
    CY_REQUIRE(handle.has_value());

    CY_REQUIRE(publish_identity(world, *handle, 4, 1.0F).has_value());
    CY_REQUIRE(publish_identity(world, *handle, 4, 2.0F).has_value());
    CY_CHECK_NEAR(world.current(*handle)[0].translation().y, 2.0F, 1e-6);
    // "the previous frame's bone matrices SHALL be available in the pose world" — what motion
    // vectors read.
    CY_CHECK_NEAR(world.previous(*handle)[0].translation().y, 1.0F, 1e-6);
    CY_CHECK_NE(world.matrix_offset(*handle), world.previous_offset(*handle));

    Array<Vec3> velocities(allocator());
    CY_REQUIRE(velocities.resize(4).has_value());
    CY_REQUIRE(world.velocities(*handle, 0.5F, velocities.span()).has_value());
    CY_CHECK_NEAR(velocities[0].y, 2.0F, 1e-5);
    // A velocity over no time is not a velocity.
    CY_CHECK_FALSE(world.velocities(*handle, 0.0F, velocities.span()).has_value());

    // Fewer matrices than the instance has bones is a caller error, not a partial publish.
    Array<Mat4> short_pose(allocator());
    CY_REQUIRE(short_pose.resize(2).has_value());
    CY_CHECK_FALSE(world.publish(*handle, short_pose.span()).has_value());
}

CY_TEST_CASE("pose world: the upload range covers what was published and nothing else") {
    PoseWorld world(allocator());
    Expected<PoseHandle, Error> first = world.add(4);
    Expected<PoseHandle, Error> second = world.add(4);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    world.clear_upload_range();
    CY_CHECK_EQ(world.upload_size(), 0U);

    CY_REQUIRE(publish_identity(world, *second, 4, 3.0F).has_value());
    CY_CHECK_EQ(world.upload_offset(), world.matrix_offset(*second));
    CY_CHECK_EQ(world.upload_size(), 4U);

    // A second publish widens the range to cover both rather than reporting the last one only: the
    // renderer transfers one span.
    CY_REQUIRE(publish_identity(world, *first, 4, 4.0F).has_value());
    CY_CHECK_EQ(world.upload_offset(), world.matrix_offset(*first));
    CY_CHECK_GE(world.upload_size(), 8U);
    CY_CHECK_LE(static_cast<usize>(world.upload_offset()) + world.upload_size(),
                world.matrices().size());

    world.clear_upload_range();
    CY_CHECK_EQ(world.upload_size(), 0U);
}

CY_TEST_CASE("pose world: a pose evaluated on a skeleton reaches the world as skinning matrices") {
    Skeleton skeleton(allocator());
    CY_REQUIRE(build_biped(skeleton).has_value());

    PoseWorld world(allocator());
    Expected<PoseHandle, Error> handle = world.add(kJointCount);
    CY_REQUIRE(handle.has_value());

    Array<Transform> local(allocator());
    Array<Transform> model(allocator());
    Array<Mat4> matrices(allocator());
    CY_REQUIRE(local.resize(kJointCount).has_value());
    CY_REQUIRE(model.resize(kJointCount).has_value());
    CY_REQUIRE(matrices.resize(kJointCount).has_value());
    skeleton.reference_pose(local.span());

    // The bind pose skins to the identity, so the world holds identities.
    CY_REQUIRE(
        publish_pose(skeleton, local.span(), 0, world, *handle, model.span(), matrices.span())
            .has_value());
    CY_CHECK_NEAR(world.current(*handle)[kFoot].translation().y, 0.0F, 1e-4);

    // Lift the character a metre and the skinning matrices carry it.
    local[kRoot].translation = Vec3{0.0F, 1.0F, 0.0F};
    CY_REQUIRE(
        publish_pose(skeleton, local.span(), 0, world, *handle, model.span(), matrices.span())
            .has_value());
    CY_CHECK_NEAR(world.current(*handle)[kFoot].translation().y, 1.0F, 1e-4);
    CY_CHECK_NEAR(world.previous(*handle)[kFoot].translation().y, 0.0F, 1e-4);

    // An instance registered with fewer bones than the skeleton has is refused rather than writing
    // past its range.
    Expected<PoseHandle, Error> small = world.add(4);
    CY_REQUIRE(small.has_value());
    CY_CHECK_FALSE(
        publish_pose(skeleton, local.span(), 0, world, *small, model.span(), matrices.span())
            .has_value());
}

CY_TEST_CASE("pose world: a skinning descriptor built from a real offset is accepted") {
    // THIS IS THE JOIN M6 LEFT OPEN. `SkinningDescriptor::validate()` refused
    // `PoseSource::GpuPoseWorld` naming the milestone that owed the world; this case builds a
    // descriptor from an offset the world actually issued and asserts the refusal is gone.
    Skeleton skeleton(allocator());
    CY_REQUIRE(build_biped(skeleton).has_value());
    PoseWorld world(allocator());
    Expected<PoseHandle, Error> filler = world.add(64);
    Expected<PoseHandle, Error> handle = world.add(kJointCount);
    CY_REQUIRE(filler.has_value());
    CY_REQUIRE(handle.has_value());

    render::geometry::SkinningDescriptor descriptor;
    descriptor.vertex_count = 4096;
    descriptor.bone_count = kJointCount;
    descriptor.retained_bones = kJointCount;
    descriptor.source = render::geometry::PoseSource::GpuPoseWorld;
    descriptor.pose_offset = world.matrix_offset(*handle);
    CY_CHECK_GT(descriptor.pose_offset, 0U);
    CY_CHECK(descriptor.validate().has_value());

    // A reduced bone level of detail is a smaller retained set and a mesh level that addresses only
    // those joints.
    render::geometry::SkinningDescriptor reduced = descriptor;
    reduced.tier = render::geometry::AnimationTier::ReducedBones;
    reduced.retained_bones = skeleton.retained_count(2);
    CY_CHECK_LT(reduced.retained_bones, static_cast<u32>(kJointCount));
    CY_CHECK(reduced.validate().has_value());

    // And the two ways of naming the world wrongly are still refused.
    render::geometry::SkinningDescriptor baked = descriptor;
    baked.tier = render::geometry::AnimationTier::Baked;
    baked.retained_bones = 0;
    CY_CHECK_FALSE(baked.validate().has_value());
    render::geometry::SkinningDescriptor confused = descriptor;
    confused.source = render::geometry::PoseSource::UploadedPerSkin;
    CY_CHECK_FALSE(confused.validate().has_value());
}
