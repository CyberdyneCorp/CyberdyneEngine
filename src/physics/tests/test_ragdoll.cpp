// SPDX-License-Identifier: MIT
#include <cy/animation/skeleton.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/physics/ragdoll/ragdoll.h>
#include <cy/servers/physics/reference/server.h>
#include <cy/test/test.h>

#if defined(CY_PHYSICS)
#    include <cy/backends/physics/jolt/server.h>
#endif

#include <array>
#include <cmath>

using namespace cy;
using namespace cy::physics;
using namespace cy::physics::ragdoll;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Physics);
}

animation::Skeleton make_skeleton() noexcept {
    animation::Skeleton skeleton(allocator());
    const auto root =
        skeleton.add_joint(Name::intern("root"), animation::kInvalidJoint, Transform::identity());
    CY_REQUIRE(root.has_value());
    const auto shoulder = skeleton.add_joint(Name::intern("shoulder"), *root,
                                             Transform::from_translation(Vec3{0.0f, 1.0f, 0.0f}));
    CY_REQUIRE(shoulder.has_value());
    const auto arm = skeleton.add_joint(Name::intern("arm"), *shoulder,
                                        Transform::from_translation(Vec3{0.4f, 0.0f, 0.0f}));
    CY_REQUIRE(arm.has_value());
    const auto hand = skeleton.add_joint(Name::intern("hand"), *arm,
                                         Transform::from_translation(Vec3{0.4f, 0.0f, 0.0f}));
    CY_REQUIRE(hand.has_value());
    for (u16 index = 0; index < 4; ++index) {
        CY_REQUIRE(
            skeleton.set_bounds(index, Aabb::from_center_extents(Vec3{}, Vec3{0.07f, 0.07f, 0.07f}))
                .has_value());
    }
    CY_REQUIRE(skeleton.finalize().has_value());
    return skeleton;
}

std::array<Transform, 4> bind_pose(const animation::Skeleton& skeleton) noexcept {
    std::array<Transform, 4> pose;
    for (u16 index = 0; index < 4; ++index) {
        pose[index] = skeleton.bind_model()[index];
    }
    return pose;
}

#if defined(CY_PHYSICS)
struct JoltFixture {
    explicit JoltFixture(u32 body_capacity = 128) noexcept {
        const auto made = jolt::create_server(allocator(), nullptr);
        CY_REQUIRE(made.has_value());
        server = *made;
        CY_REQUIRE(server->initialize().has_value());
        WorldDescription description;
        description.name = Name::intern("ragdoll-test");
        description.body_capacity = body_capacity;
        description.body_pair_capacity = 512;
        description.contact_constraint_capacity = 512;
        const auto created = server->create_world(description);
        CY_REQUIRE(created.has_value());
        world = *created;
        CY_REQUIRE(server->set_gravity(world, Vec3{}).has_value());
    }

    ~JoltFixture() {
        if (server != nullptr) {
            (void)server->destroy_world(world);
            server->shutdown();
            jolt::destroy_server(server, allocator());
        }
    }

    JoltFixture(const JoltFixture&) = delete;
    JoltFixture& operator=(const JoltFixture&) = delete;

    void step(u64 tick) const noexcept {
        StepInput input;
        input.tick = tick;
        CY_REQUIRE(server->step(world, input).has_value());
    }

    PhysicsServer* server = nullptr;
    WorldHandle world;
};
#endif

}  // namespace

CY_TEST_CASE("a ragdoll profile derives editable shapes and limits from a finalized skeleton") {
    animation::Skeleton unfinished(allocator());
    CY_CHECK_EQ(Profile::generate(unfinished, allocator()).error().code,
                ErrorCode::InvalidArgument);
    animation::Skeleton skeleton = make_skeleton();
    auto generated = Profile::generate(skeleton, allocator());
    CY_REQUIRE(generated.has_value());
    CY_CHECK_EQ(generated->bones().size(), 4U);
    CY_CHECK_EQ(generated->bones()[0].shape.type, ShapeType::Sphere);
    CY_CHECK_EQ(generated->bones()[2].shape.type, ShapeType::Capsule);
    CY_CHECK_GT(generated->bones()[2].shape.half_height, 0.0f);
    CY_CHECK_GT(generated->bones()[2].mass, 0.0f);
    CY_CHECK(generated->bones()[2].twist_limit.limited());
    generated->bones()[2].motor_torque = 12.0f;
    CY_CHECK_EQ(generated->bones()[2].motor_torque, 12.0f);
}

CY_TEST_CASE("a reference backend refuses ragdoll activation before allocating bodies") {
    animation::Skeleton skeleton = make_skeleton();
    auto profile = Profile::generate(skeleton, allocator());
    CY_REQUIRE(profile.has_value());
    const auto made = reference::create_server(allocator());
    CY_REQUIRE(made.has_value());
    PhysicsServer* server = *made;
    CY_REQUIRE(server->initialize().has_value());
    WorldDescription description;
    description.name = Name::intern("ragdoll-reference");
    const auto world = server->create_world(description);
    CY_REQUIRE(world.has_value());
    {
        Ragdoll ragdoll(*server, *world, skeleton, *profile, allocator());
        const auto pose = bind_pose(skeleton);
        const Status activated = ragdoll.activate(pose, pose, Transform::identity(),
                                                  Transform::identity(), 1.0f / 60.0f, {});
        CY_CHECK_EQ(activated.error().code, ErrorCode::Unsupported);
        CY_CHECK_FALSE(ragdoll.active());
        CY_CHECK(ragdoll.body(0).is_null());
    }
    CY_REQUIRE(server->destroy_world(*world).has_value());
    server->shutdown();
    reference::destroy_server(server, allocator());
}

#if defined(CY_PHYSICS)
CY_TEST_CASE("full ragdoll seeds animated pose and world velocity, then blends continuously") {
    JoltFixture fixture;
    animation::Skeleton skeleton = make_skeleton();
    auto profile = Profile::generate(skeleton, allocator());
    CY_REQUIRE(profile.has_value());
    profile->bones()[2].mass = 2.5f;
    Ragdoll ragdoll(*fixture.server, fixture.world, skeleton, *profile, allocator());
    const auto pose = bind_pose(skeleton);
    auto previous_pose = pose;
    previous_pose[2].rotation = Quat::from_axis_angle(kAxisX, 0.2f);
    const Transform actor = Transform::from_translation(Vec3{1.0f, 0.0f, 0.0f});
    Activation activation;
    activation.blend_seconds = 1.0f;
    CY_REQUIRE(ragdoll.activate(pose, previous_pose, actor, Transform::identity(), 1.0f, activation)
                   .has_value());
    CY_CHECK_LT(length(fixture.server->body_state(ragdoll.body(0))->transform.translation -
                       actor.translation),
                0.01f);
    CY_CHECK_LT(length(fixture.server->body_state(ragdoll.body(0))->linear_velocity -
                       Vec3{1.0f, 0.0f, 0.0f}),
                0.01f);
    CY_CHECK_EQ(fixture.server->mass_properties(ragdoll.body(2))->mass, 2.5f);
    CY_CHECK_LT(std::fabs(fixture.server->body_state(ragdoll.body(2))->angular_velocity.x + 0.2f),
                0.01f);
    std::array<Transform, 4> output;
    Transform displaced = fixture.server->body_state(ragdoll.body(0))->transform;
    displaced.translation.x += 2.0f;
    CY_REQUIRE(
        fixture.server->set_body_transform(ragdoll.body(0), displaced, TeleportMode::Teleport)
            .has_value());
    CY_REQUIRE(ragdoll.sample_pose(pose, actor, output).has_value());
    CY_CHECK_EQ(output[0].translation, pose[0].translation);
    CY_CHECK_EQ(ragdoll.blend_weight(2), 0.0f);
    CY_REQUIRE(ragdoll.advance_blend(0.5f).has_value());
    CY_CHECK_EQ(ragdoll.blend_weight(2), 0.5f);
    CY_REQUIRE(ragdoll.sample_pose(pose, actor, output).has_value());
    CY_CHECK_LT(std::fabs(output[0].translation.x - 1.0f), 0.01f);
    CY_REQUIRE(ragdoll.advance_blend(0.5f).has_value());
    CY_CHECK_EQ(ragdoll.blend_weight(2), 1.0f);
    CY_REQUIRE(ragdoll.sample_pose(pose, actor, output).has_value());
    CY_CHECK_LT(std::fabs(output[0].translation.x - 2.0f), 0.01f);
    const BodyHandle root = ragdoll.body(0);
    CY_REQUIRE(ragdoll.deactivate().has_value());
    CY_CHECK_FALSE(fixture.server->body_alive(root));
    const u32 unique_before = fixture.server->shape_statistics()->unique_shapes;
    const auto reopened = fixture.server->create_shape(profile->bones()[0].shape);
    CY_REQUIRE(reopened.has_value());
    CY_CHECK_EQ(fixture.server->shape_statistics()->unique_shapes, unique_before + 1U);
    CY_REQUIRE(fixture.server->destroy_shape(*reopened).has_value());
    CY_REQUIRE(ragdoll.activate(pose, previous_pose, actor, Transform::identity(), 1.0f, activation)
                   .has_value());
    CY_CHECK(ragdoll.body(0) != root);
}

CY_TEST_CASE("a ragdoll that exceeds world capacity rolls back every body and shape") {
    JoltFixture fixture(2);
    animation::Skeleton skeleton = make_skeleton();
    auto profile = Profile::generate(skeleton, allocator());
    CY_REQUIRE(profile.has_value());
    Ragdoll ragdoll(*fixture.server, fixture.world, skeleton, *profile, allocator());
    const auto pose = bind_pose(skeleton);
    const Status activated = ragdoll.activate(pose, pose, Transform::identity(),
                                              Transform::identity(), 1.0f / 60.0f, {});
    CY_CHECK_EQ(activated.error().code, ErrorCode::OutOfRange);
    CY_CHECK_FALSE(ragdoll.active());
    CY_CHECK(ragdoll.body(0).is_null());
    const u32 unique_before = fixture.server->shape_statistics()->unique_shapes;
    const auto reopened = fixture.server->create_shape(profile->bones()[0].shape);
    CY_REQUIRE(reopened.has_value());
    CY_CHECK_EQ(fixture.server->shape_statistics()->unique_shapes, unique_before + 1U);
    CY_REQUIRE(fixture.server->destroy_shape(*reopened).has_value());
}

CY_TEST_CASE("full ragdoll ignores subsequent animation targets instead of becoming powered") {
    JoltFixture fixture;
    animation::Skeleton skeleton = make_skeleton();
    auto profile = Profile::generate(skeleton, allocator());
    CY_REQUIRE(profile.has_value());
    Ragdoll ragdoll(*fixture.server, fixture.world, skeleton, *profile, allocator());
    const auto pose = bind_pose(skeleton);
    CY_REQUIRE(
        ragdoll.activate(pose, pose, Transform::identity(), Transform::identity(), 1.0f / 60.0f, {})
            .has_value());
    auto target = pose;
    target[2].rotation = Quat::from_axis_angle(kAxisX, 0.6f);
    CY_REQUIRE(
        ragdoll.set_animation_target(target, Transform::identity(), 1.0f / 60.0f).has_value());
    for (u64 tick = 0; tick < 30; ++tick) {
        fixture.step(tick);
    }
    CY_CHECK_GT(angle_between(fixture.server->body_state(ragdoll.body(2))->transform.rotation,
                              target[2].rotation),
                0.4f);
}

CY_TEST_CASE("powered ragdoll preserves animated root velocity at activation") {
    JoltFixture fixture;
    animation::Skeleton skeleton = make_skeleton();
    auto profile = Profile::generate(skeleton, allocator());
    CY_REQUIRE(profile.has_value());
    Ragdoll ragdoll(*fixture.server, fixture.world, skeleton, *profile, allocator());
    const auto pose = bind_pose(skeleton);
    Activation activation;
    activation.mode = Mode::Powered;
    const Transform actor = Transform::from_translation(Vec3{1.0f, 0.0f, 0.0f});
    CY_REQUIRE(
        ragdoll.activate(pose, pose, actor, Transform::identity(), 1.0f, activation).has_value());
    const auto root = fixture.server->body_state(ragdoll.body(0));
    CY_REQUIRE(root.has_value());
    CY_CHECK_EQ(root->motion, MotionType::Kinematic);
    CY_CHECK_LT(length(root->linear_velocity - Vec3{1.0f, 0.0f, 0.0f}), 0.01f);
}

CY_TEST_CASE("invalid hand-refined ragdoll limits fail before creating solver bodies") {
    JoltFixture fixture;
    animation::Skeleton skeleton = make_skeleton();
    auto profile = Profile::generate(skeleton, allocator());
    CY_REQUIRE(profile.has_value());
    profile->bones()[2].swing_limit_y = -0.1f;
    Ragdoll ragdoll(*fixture.server, fixture.world, skeleton, *profile, allocator());
    const auto pose = bind_pose(skeleton);
    const Status activated = ragdoll.activate(pose, pose, Transform::identity(),
                                              Transform::identity(), 1.0f / 60.0f, {});
    CY_CHECK_EQ(activated.error().code, ErrorCode::InvalidArgument);
    CY_CHECK(ragdoll.body(0).is_null());
    CY_CHECK_EQ(fixture.server->shape_statistics()->requests, 0U);
}

CY_TEST_CASE("partial ragdoll keeps locomotion animated while the arm reacts and blends back") {
    JoltFixture fixture;
    animation::Skeleton skeleton = make_skeleton();
    auto profile = Profile::generate(skeleton, allocator());
    CY_REQUIRE(profile.has_value());
    Ragdoll ragdoll(*fixture.server, fixture.world, skeleton, *profile, allocator());
    auto pose = bind_pose(skeleton);
    const std::array<f32, 4> weights{0.0f, 0.0f, 0.5f, 1.0f};
    Activation activation;
    activation.mode = Mode::Partial;
    activation.partial_weights = weights;
    CY_REQUIRE(ragdoll
                   .activate(pose, pose, Transform::identity(), Transform::identity(), 1.0f / 60.0f,
                             activation)
                   .has_value());
    CY_CHECK_EQ(fixture.server->body_state(ragdoll.body(0))->motion, MotionType::Kinematic);
    CY_CHECK_EQ(fixture.server->body_state(ragdoll.body(3))->motion, MotionType::Dynamic);
    pose[0].translation.x += 0.5f;
    for (u16 index = 1; index < 4; ++index) {
        pose[index].translation.x += 0.5f;
    }
    CY_REQUIRE(ragdoll.set_animation_target(pose, Transform::identity(), 1.0f / 60.0f).has_value());
    CY_CHECK_LT(length(fixture.server->body_state(ragdoll.body(0))->transform.translation -
                       pose[0].translation),
                0.01f);
    CY_REQUIRE(ragdoll.apply_hit(3, Vec3{0.0f, 0.0f, 3.0f}, 0.5f).has_value());
    CY_CHECK_EQ(ragdoll.blend_weight(3), 1.0f);
    for (u64 tick = 0; tick < 10; ++tick) {
        fixture.step(tick);
    }
    std::array<Transform, 4> output;
    CY_REQUIRE(ragdoll.sample_pose(pose, Transform::identity(), output).has_value());
    CY_CHECK_EQ(output[0].translation, pose[0].translation);
    CY_CHECK_GT(std::fabs(output[3].translation.z - pose[3].translation.z), 0.01f);
    CY_REQUIRE(ragdoll.advance_blend(0.25f).has_value());
    CY_CHECK_EQ(ragdoll.blend_weight(2), 0.5f);
}

CY_TEST_CASE("powered ragdoll motor tracks animation after an impulse and hit weight recovers") {
    JoltFixture fixture;
    animation::Skeleton skeleton = make_skeleton();
    auto profile = Profile::generate(skeleton, allocator());
    CY_REQUIRE(profile.has_value());
    Ragdoll ragdoll(*fixture.server, fixture.world, skeleton, *profile, allocator());
    auto pose = bind_pose(skeleton);
    Activation activation;
    activation.mode = Mode::Powered;
    CY_REQUIRE(ragdoll
                   .activate(pose, pose, Transform::identity(), Transform::identity(), 1.0f / 60.0f,
                             activation)
                   .has_value());
    pose[2].rotation = Quat::from_axis_angle(kAxisX, 0.35f);
    pose[3].rotation = pose[2].rotation;
    CY_REQUIRE(ragdoll.set_animation_target(pose, Transform::identity(), 1.0f / 60.0f).has_value());
    for (u64 tick = 0; tick < 40; ++tick) {
        fixture.step(tick);
    }
    const f32 before = angle_between(
        fixture.server->body_state(ragdoll.body(2))->transform.rotation, pose[2].rotation);
    CY_CHECK_LT(before, 0.25f);
    CY_REQUIRE(ragdoll.apply_hit(2, Vec3{0.0f, 0.0f, 5.0f}, 1.0f).has_value());
    CY_CHECK_EQ(ragdoll.blend_weight(2), 1.0f);
    fixture.step(40);
    const f32 displacement = length(
        fixture.server->body_state(ragdoll.body(2))->transform.translation - pose[2].translation);
    CY_CHECK_GT(displacement, 0.001f);
    for (u64 tick = 41; tick < 101; ++tick) {
        fixture.step(tick);
        CY_REQUIRE(ragdoll.advance_blend(1.0f / 60.0f).has_value());
    }
    const f32 recovered = length(
        fixture.server->body_state(ragdoll.body(2))->transform.translation - pose[2].translation);
    CY_CHECK_LT(recovered, displacement);
    CY_CHECK_LT(ragdoll.blend_weight(2), 0.01f);
    CY_CHECK_LT(angle_between(fixture.server->body_state(ragdoll.body(2))->transform.rotation,
                              pose[2].rotation),
                0.3f);
}
#endif
