// SPDX-License-Identifier: MIT
// Joints authored in the editor and simulated at play. Issue #29, the physics tools.
//
// The world below is written out in full for the reason test_play.cpp gives: its type and field
// names are a contract with `editor/crates/cy-editor-services/src/joints.rs`, whose tests hold the
// same golden text. A pendulum: a static anchor, and a ball one metre beside it joined to it by a
// point joint whose anchor is at the anchor body's centre. Without the joint the ball falls; with
// it the ball swings on a one-metre arm.

#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/play/joints.h>
#include <cy/gameplay/play/session.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/servers/physics/reference/server.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#if defined(CY_PHYSICS)
#    include <cy/backends/physics/jolt/server.h>
#endif

#include <string>
#include <string_view>

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;
using namespace cy::gameplay;

namespace ser = cy::scene::serialization;
namespace physics = cy::physics;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

constexpr std::string_view kAssetPath = "worlds/pendulum.cyworld";

/// The pendulum. `joint` is the Joint component's field lines, so a case can author another kind.
[[nodiscard]] std::string pendulum(std::string_view joint) {
    std::string text =
        "cyworld 1\n"
        "type 1 runtime \"Transform\"\n"
        "  field 1 vec3 \"translation\" \"\"\n"
        "  field 2 quat \"rotation\" \"\"\n"
        "  field 3 vec3 \"scale\" \"\"\n"
        "type 2 runtime \"StaticBody\"\n"
        "type 3 runtime \"RigidBody\"\n"
        "  field 4 float \"mass\" \"\"\n"
        "  field 5 float \"gravity_scale\" \"\"\n"
        "type 4 runtime \"Collider\"\n"
        "  field 6 text \"shape\" \"\"\n"
        "  field 7 vec3 \"extent\" \"\"\n"
        "  field 8 float \"radius\" \"\"\n"
        "  field 9 float \"height\" \"\"\n"
        "type 5 runtime \"Joint\"\n"
        "  field 10 text \"kind\" \"\"\n"
        "  field 11 entity \"target\" \"\"\n"
        "  field 12 vec3 \"anchor\" \"\"\n"
        "  field 13 vec3 \"axis\" \"\"\n"
        "  field 14 float \"limit_min\" \"\"\n"
        "  field 15 float \"limit_max\" \"\"\n"
        "  field 16 float \"motor_velocity\" \"\"\n"
        "  field 17 float \"motor_max_force\" \"\"\n"
        "node 0 - \"default\" \"Anchor\"\n"
        "  component 1\n"
        "    field 1 0 5 0\n"
        "    field 2 0 0 0 1\n"
        "    field 3 1 1 1\n"
        "  component 4\n"
        "    field 6 \"box\"\n"
        "    field 7 0.1 0.1 0.1\n"
        "    field 8 0.5\n"
        "    field 9 1\n"
        "  component 2\n"
        "node 1 - \"default\" \"Bob\"\n"
        "  component 1\n"
        "    field 1 1 5 0\n"
        "    field 2 0 0 0 1\n"
        "    field 3 1 1 1\n"
        "  component 4\n"
        "    field 6 \"sphere\"\n"
        "    field 7 0.2 0.2 0.2\n"
        "    field 8 0.2\n"
        "    field 9 1\n"
        "  component 3\n"
        "    field 4 1\n"
        "    field 5 1\n";
    text += joint;
    return text;
}

/// A point joint from the bob to the anchor node, anchored at the anchor's centre.
constexpr std::string_view kPointJoint =
    "  component 5\n"
    "    field 10 \"point\"\n"
    "    field 11 0\n"
    "    field 12 -1 0 0\n"
    "    field 13 1 0 0\n";

struct Authored {
    explicit Authored(std::string_view joint) noexcept : world(allocator()), schema(allocator()) {
        started = cy::reflect::register_scene_types(registry).has_value() &&
                  ser::build_authoring_schema(registry, schema).has_value();
        text = pendulum(joint);
        started = started && ser::read_world(text, kAssetPath, world).has_value() &&
                  ser::resolve_against(world, schema).has_value();
    }

    std::string text;
    cy::reflect::TypeRegistry registry;
    ser::World world;
    ser::AuthoringSchema schema;
    bool started = false;
};

[[nodiscard]] PlayConfiguration configuration_over(physics::PhysicsServer* server,
                                                   const ser::AuthoringSchema& schema) {
    PlayConfiguration configuration;
    configuration.physics = server;
    configuration.schema = &schema;
    configuration.body_capacity = 16;
    return configuration;
}

[[nodiscard]] Vec3 position_of(const ser::World& world, u32 index) {
    cy::Transform placement;
    if (!ser::transform_of(world, world.nodes()[index], placement)) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }
    return placement.translation;
}

/// The bob's joint as `authored_joint` reads it, for the cases about the mapping alone.
[[nodiscard]] AuthoredJoint read_joint(std::string_view fields) {
    Authored authored(fields);
    CY_REQUIRE(authored.started);
    const cy::Expected<AuthoredJoint, cy::Error> joint =
        authored_joint(authored.world, authored.world.nodes()[1]);
    CY_REQUIRE(joint.has_value());
    return *joint;
}

}  // namespace

// --- Reading what the editor wrote --------------------------------------------------------------

CY_TEST_CASE("every joint kind the editor can name is one the engine has") {
    const char* const words[] = {"fixed", "point",       "hinge",   "slider",          "distance",
                                 "cone",  "swing-twist", "six-dof", "rack-and-pinion", "gear"};
    u32 index = 0;
    for (const char* word : words) {
        const cy::Expected<physics::ConstraintType, cy::Error> kind = joint_kind_of(word);
        CY_REQUIRE(kind.has_value());
        CY_CHECK_EQ(static_cast<u32>(*kind), index);
        ++index;
    }
    CY_CHECK_FALSE(joint_kind_of("ball").has_value());
}

CY_TEST_CASE("a point joint names its target and its anchor in the body's frame") {
    Authored authored(kPointJoint);
    CY_REQUIRE(authored.started);
    const cy::Expected<AuthoredJoint, cy::Error> joint =
        authored_joint(authored.world, authored.world.nodes()[1]);
    CY_REQUIRE(joint.has_value());
    CY_CHECK(joint->description.type == physics::ConstraintType::Point);
    CY_CHECK_EQ(joint->target, authored.world.nodes()[0].identity);
    CY_CHECK_NEAR(joint->description.frame_a.translation.x, -1.0F, 1e-6);
    CY_CHECK(joint->description.body_a.is_null());

    // The anchor node carries no joint, which is not an error.
    const cy::Expected<AuthoredJoint, cy::Error> none =
        authored_joint(authored.world, authored.world.nodes()[0]);
    CY_REQUIRE_FALSE(none.has_value());
    CY_CHECK(none.error().code == cy::ErrorCode::NotFound);
}

CY_TEST_CASE("a hinge reads its range and its motor, and its axis turns the frame") {
    const AuthoredJoint hinge = read_joint(
        "  component 5\n"
        "    field 10 \"hinge\"\n"
        "    field 11 -\n"
        "    field 13 0 0 1\n"
        "    field 14 -0.5\n"
        "    field 15 0.75\n"
        "    field 16 2\n"
        "    field 17 40\n");
    CY_CHECK(hinge.description.type == physics::ConstraintType::Hinge);
    CY_CHECK_EQ(hinge.target, 0ULL);
    CY_CHECK_NEAR(hinge.description.limit.min, -0.5F, 1e-6);
    CY_CHECK_NEAR(hinge.description.limit.max, 0.75F, 1e-6);
    CY_CHECK_NEAR(hinge.description.motor.target_velocity, 2.0F, 1e-6);
    CY_CHECK_NEAR(hinge.description.motor.max_force, 40.0F, 1e-6);
    // The frame's local X is the hinge axis, so an authored Z axis turns X onto Z.
    const Vec3 axis = hinge.description.frame_a.right();
    CY_CHECK_NEAR(axis.z, 1.0F, 1e-5);
    CY_CHECK_NEAR(axis.x, 0.0F, 1e-5);
}

CY_TEST_CASE("the one range is the twist of a swing-twist and the span of a distance joint") {
    const AuthoredJoint twist = read_joint(
        "  component 5\n"
        "    field 10 \"swing-twist\"\n"
        "    field 14 -0.2\n"
        "    field 15 0.3\n");
    CY_CHECK_NEAR(twist.description.twist_limit.min, -0.2F, 1e-6);
    CY_CHECK_NEAR(twist.description.twist_limit.max, 0.3F, 1e-6);
    CY_CHECK_FALSE(twist.description.limit.limited());

    const AuthoredJoint rod = read_joint(
        "  component 5\n"
        "    field 10 \"distance\"\n"
        "    field 14 0.5\n"
        "    field 15 2\n");
    CY_CHECK_NEAR(rod.description.min_distance, 0.5F, 1e-6);
    CY_CHECK_NEAR(rod.description.max_distance, 2.0F, 1e-6);
}

CY_TEST_CASE("a joint of a kind this build does not know is refused by name") {
    Authored authored(
        "  component 5\n"
        "    field 10 \"ball\"\n");
    CY_REQUIRE(authored.started);
    const cy::Expected<AuthoredJoint, cy::Error> joint =
        authored_joint(authored.world, authored.world.nodes()[1]);
    CY_REQUIRE_FALSE(joint.has_value());
    CY_CHECK(joint.error().code == cy::ErrorCode::InvalidArgument);
}

CY_TEST_CASE("frame B is chosen so both anchors coincide where the bodies were authored") {
    cy::Transform body_a = cy::Transform::from_translation(Vec3{1.0F, 5.0F, 0.0F});
    body_a.rotation = cy::Quat::from_axis_angle(Vec3{0.0F, 1.0F, 0.0F}, 0.7F);
    const cy::Transform frame_a = cy::Transform::from_translation(Vec3{-1.0F, 0.0F, 0.0F});
    const cy::Transform body_b = cy::Transform::from_translation(Vec3{0.0F, 5.0F, 0.0F});
    const cy::Transform frame_b = frame_on_target(body_a, frame_a, &body_b);
    const Vec3 anchor_a = (body_a * frame_a).translation;
    const Vec3 anchor_b = (body_b * frame_b).translation;
    CY_CHECK_NEAR(anchor_a.x, anchor_b.x, 1e-5);
    CY_CHECK_NEAR(anchor_a.y, anchor_b.y, 1e-5);
    CY_CHECK_NEAR(anchor_a.z, anchor_b.z, 1e-5);

    // Joined to the world, frame B IS the world placement of the anchor.
    const cy::Transform world = frame_on_target(body_a, frame_a, nullptr);
    CY_CHECK_NEAR(world.translation.x, anchor_a.x, 1e-6);
}

// --- Handing them to the bridge ------------------------------------------------------------------

CY_TEST_CASE("play hands an authored joint to the bridge, which defers it on a solver without") {
    Authored authored(kPointJoint);
    CY_REQUIRE(authored.started);
    const cy::Expected<physics::PhysicsServer*, cy::Error> made =
        physics::reference::create_server(allocator());
    CY_REQUIRE(made.has_value());
    CY_REQUIRE((*made)->initialize().has_value());
    {
        PlaySession session(allocator(), authored.world);
        CY_REQUIRE(session.enter(configuration_over(*made, authored.schema)).has_value());
        CY_CHECK_EQ(session.report().joints, 1U);
        CY_CHECK_EQ(session.report().joints_refused, 0U);
        // The reference backend has no constraints and says so; the joint waits rather than being
        // created half-way or dropped.
        CY_CHECK_FALSE((*made)->capabilities().constraints);
        CY_CHECK_EQ(session.bridge()->statistics().joints_created, 0U);
        CY_CHECK_EQ(session.bridge()->statistics().joints_deferred, 1U);
        CY_REQUIRE(session.stop().has_value());
        CY_CHECK(session.report().restored_exactly);
    }
    (*made)->shutdown();
    physics::reference::destroy_server(*made, allocator());
}

CY_TEST_CASE("a joint whose target has no body is counted as refused and the world still plays") {
    // The anchor loses its body: the joint names a node the solver has nothing for.
    std::string text = pendulum(kPointJoint);
    const std::string_view static_body = "  component 2\n";
    text.erase(text.find(static_body), static_body.size());
    cy::reflect::TypeRegistry registry;
    ser::AuthoringSchema schema(allocator());
    CY_REQUIRE(cy::reflect::register_scene_types(registry).has_value());
    CY_REQUIRE(ser::build_authoring_schema(registry, schema).has_value());
    ser::World world(allocator());
    CY_REQUIRE(ser::read_world(text, kAssetPath, world).has_value());
    CY_REQUIRE(ser::resolve_against(world, schema).has_value());
    const cy::Expected<physics::PhysicsServer*, cy::Error> made =
        physics::reference::create_server(allocator());
    CY_REQUIRE(made.has_value());
    CY_REQUIRE((*made)->initialize().has_value());
    {
        PlaySession session(allocator(), world);
        CY_REQUIRE(session.enter(configuration_over(*made, schema)).has_value());
        CY_CHECK_EQ(session.report().joints, 0U);
        CY_CHECK_EQ(session.report().joints_refused, 1U);
        CY_CHECK(session.tick().has_value());
        CY_REQUIRE(session.stop().has_value());
    }
    (*made)->shutdown();
    physics::reference::destroy_server(*made, allocator());
}

#if defined(CY_PHYSICS)

namespace {

/// Where the bob is after two seconds of play on Jolt, with `joint` authored on it.
[[nodiscard]] Vec3 bob_after_two_seconds(std::string_view joint, u64* created) {
    Authored authored(joint);
    CY_REQUIRE(authored.started);
    const cy::Expected<physics::PhysicsServer*, cy::Error> made =
        physics::jolt::create_server(allocator(), nullptr);
    CY_REQUIRE(made.has_value());
    CY_REQUIRE((*made)->initialize().has_value());
    Vec3 bob{};
    {
        PlaySession session(allocator(), authored.world);
        CY_REQUIRE(session.enter(configuration_over(*made, authored.schema)).has_value());
        for (u32 tick = 0; tick < 120; ++tick) {
            CY_REQUIRE(session.tick().has_value());
        }
        bob = position_of(authored.world, 1);
        *created = session.bridge()->statistics().joints_created;
        CY_REQUIRE(session.stop().has_value());
    }
    (*made)->shutdown();
    physics::jolt::destroy_server(*made, allocator());
    return bob;
}

}  // namespace

CY_TEST_CASE("an authored point joint holds the bob on its arm while it swings") {
    u64 created = 0;
    const Vec3 bob = bob_after_two_seconds(kPointJoint, &created);
    CY_CHECK_EQ(created, 1ULL);
    // A one-metre arm about (0, 5, 0): the bob swings below the anchor and never leaves the arm.
    const Vec3 arm{bob.x, bob.y - 5.0F, bob.z};
    CY_CHECK_NEAR(cy::length(arm), 1.0F, 0.05);
    CY_CHECK(bob.y > 3.9F);

    // The same world with no joint is the control: the bob falls about twenty metres.
    u64 none = 0;
    const Vec3 fallen = bob_after_two_seconds("", &none);
    CY_CHECK_EQ(none, 0ULL);
    CY_CHECK(fallen.y < -5.0F);
}

#endif  // CY_PHYSICS
