// SPDX-License-Identifier: MIT
// `integration.game_backend_camera_collision` — a camera rig's batched collision and occlusion
// queries, answered by a physics world (cy/game_backend/camera_collision.h).
//
// The world is the reference backend's: a floor, a wall two and a half metres behind the target,
// and a capsule round the target standing for the character the camera frames. The rig is the
// third-person composition — a target, an orbit six metres back, a collision node — evaluated by a
// real camera server whose queries the resolver answers every frame.

#include <cy/core/math/scalar.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/camera_collision.h>
#include <cy/servers/camera/server.h>
#include <cy/servers/physics/reference/server.h>
#include <cy/test/test.h>

#include <cmath>

namespace {

using namespace cy::camera;
using cy::f32;
using cy::Name;
using cy::u32;
using cy::Vec3;
namespace physics = cy::physics;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Physics);
}

constexpr f32 kWallFace = 2.5F;
constexpr f32 kProbe = 0.25F;
constexpr Vec3 kAnchor{0.0F, 1.5F, 0.0F};

/// A static box with its centre at `centre`.
[[nodiscard]] physics::BodyHandle make_box(
    physics::PhysicsServer& server, physics::WorldHandle world, Vec3 centre, Vec3 half,
    physics::MotionType motion = physics::MotionType::Static) {
    physics::ShapeDescription box;
    box.type = physics::ShapeType::Box;
    box.half_extents = half;
    const auto shape = server.create_shape(box);
    CY_REQUIRE(shape.has_value());
    physics::ColliderDescription collider;
    collider.shape = *shape;
    physics::BodyDescription body;
    body.motion = motion;
    body.transform = cy::Transform::from_translation(centre);
    body.colliders = &collider;
    body.collider_count = 1;
    const auto made = server.create_body(world, body);
    CY_REQUIRE(made.has_value());
    return *made;
}

struct World {
    physics::PhysicsServer* server = nullptr;
    physics::WorldHandle world;
    physics::BodyHandle wall;
    physics::BodyHandle character;

    World() {
        const auto made = physics::reference::create_server(allocator());
        CY_REQUIRE(made.has_value());
        server = *made;
        CY_REQUIRE(server->initialize().has_value());
        physics::WorldDescription description;
        description.name = Name::intern("camera-collision");
        description.body_capacity = 16;
        const auto created = server->create_world(description);
        CY_REQUIRE(created.has_value());
        world = *created;
        // The wall's face toward the target is at z = 2.5, two metres high, wide.
        wall =
            make_box(*server, world, Vec3{0.0F, 1.0F, kWallFace + 0.25F}, Vec3{4.0F, 2.0F, 0.25F});
        // The character's body round the anchor.
        character = make_box(*server, world, Vec3{0.0F, 0.9F, 0.0F}, Vec3{0.35F, 0.9F, 0.35F},
                             physics::MotionType::Kinematic);
    }

    ~World() {
        server->shutdown();
        physics::reference::destroy_server(server, allocator());
    }
};

[[nodiscard]] RigNodeDesc node(const char* id, const char* input, RigNodeKind kind) noexcept {
    RigNodeDesc desc;
    desc.id = Name::intern(id);
    desc.input = input == nullptr ? Name{} : Name::intern(input);
    desc.kind = kind;
    return desc;
}

/// Target, an orbit six metres back with the yaw that puts it on +z, and a collision node.
struct Camera {
    CameraServer server{allocator()};
    RigHandle rig;

    explicit Camera(f32 probe = kProbe) {
        CY_REQUIRE(server.configure(CameraServerConfig{}).has_value());
        CY_REQUIRE(server.initialize().has_value());
        RigDefinition definition(allocator());
        definition.name = Name::intern("third-person");
        (void)definition.nodes.push_back(node("target", nullptr, RigNodeKind::Target));
        RigNodeDesc orbit = node("orbit", "target", RigNodeKind::Orbit);
        orbit.orbit.near_distance = 6.0F;
        orbit.orbit.far_distance = 6.0F;
        orbit.orbit.distance_half_life = 0.0F;
        (void)definition.nodes.push_back(orbit);
        RigNodeDesc collide = node("collide", "orbit", RigNodeKind::Collision);
        collide.collision.probe_radius = probe;
        collide.collision.collision_response = CollisionResponse::PullIn;
        collide.collision.occlusion_response = CollisionResponse::FadeObstacle;
        (void)definition.nodes.push_back(collide);
        (void)definition.nodes.push_back(node("output", "collide", RigNodeKind::Output));
        const auto compiled = server.create_definition(definition);
        CY_REQUIRE(compiled.has_value());
        const auto made = server.create_rig(*compiled, RigConfig{});
        CY_REQUIRE(made.has_value());
        rig = *made;
        TargetBinding binding;
        binding.kind = TargetKind::Position;
        binding.position = kAnchor;
        CY_REQUIRE(server.set_target(rig, binding).has_value());
    }

    /// One frame: the rig evaluated, then its queries answered for the next. The camera's distance
    /// from the anchor along +z.
    [[nodiscard]] f32 frame(cy::game_backend::CameraCollisionResolver& resolver) {
        server.begin_frame();
        EvaluationContext context;
        context.delta_seconds = 1.0F / 60.0F;
        const auto evaluated = server.evaluate(rig, context);
        CY_REQUIRE(evaluated.has_value());
        CY_REQUIRE(resolver.resolve(server).has_value());
        return (*evaluated)->pose.translation.z - kAnchor.z;
    }
};

}  // namespace

CY_TEST_CASE("game_backend: a camera's collision sweep stops at the wall behind the target") {
    World world;
    Camera camera;
    cy::game_backend::CameraCollisionResolver resolver(allocator(), *world.server, world.world);
    const physics::BodyHandle ignored[] = {world.character};
    CY_REQUIRE(resolver.set_ignored({ignored, 1}).has_value());

    // The first frame has no answers yet: the camera is where the rig wants it, and the sweep
    // toward it finds the wall.
    CY_CHECK_NEAR(camera.frame(resolver), 6.0F, 1e-3F);
    CY_CHECK_EQ(resolver.stats().collision_queries, 1U);
    CY_CHECK_EQ(resolver.stats().collisions, 1U);
    CY_CHECK_NEAR(resolver.stats().nearest_fraction, (kWallFace - kProbe) / 6.0F, 1e-3F);
    // Every frame after: in front of the wall by the probe's radius, and still.
    for (int step = 0; step < 60; ++step) {
        CY_CHECK_NEAR(camera.frame(resolver), kWallFace - kProbe, 1e-3F);
    }
    // The ray from where the rig wants the camera back to the target is blocked by the same wall.
    CY_CHECK_EQ(resolver.stats().occlusion_queries, 1U);
    CY_CHECK_EQ(resolver.stats().occlusions, 1U);
}

CY_TEST_CASE("game_backend: the framed character's own body never pulls its camera in") {
    World world;
    CY_REQUIRE(world.server->destroy_body(world.wall).has_value());
    Camera camera;
    cy::game_backend::CameraCollisionResolver resolver(allocator(), *world.server, world.world);

    // NOT ignored: the probe starts inside the character's capsule, so it is answered zero and the
    // camera comes all the way in to the target.
    (void)camera.frame(resolver);
    CY_CHECK_EQ(resolver.stats().collisions, 1U);
    CY_CHECK_NEAR(resolver.stats().nearest_fraction, 0.0F, 1e-6F);
    CY_CHECK_LT(camera.frame(resolver), 0.5F);

    // Ignored: clear, and the camera eases back out to the six metres the rig wants.
    const physics::BodyHandle ignored[] = {world.character};
    CY_REQUIRE(resolver.set_ignored({ignored, 1}).has_value());
    f32 distance = 0.0F;
    for (int step = 0; step < 300; ++step) {
        distance = camera.frame(resolver);
        CY_CHECK_EQ(resolver.stats().collisions, 0U);
    }
    CY_CHECK_NEAR(distance, 6.0F, 1e-3F);
}

CY_TEST_CASE("game_backend: a wall that is taken away lets the camera ease back out") {
    World world;
    Camera camera;
    cy::game_backend::CameraCollisionResolver resolver(allocator(), *world.server, world.world);
    const physics::BodyHandle ignored[] = {world.character};
    CY_REQUIRE(resolver.set_ignored({ignored, 1}).has_value());
    for (int step = 0; step < 30; ++step) {
        (void)camera.frame(resolver);
    }
    CY_CHECK_NEAR(camera.frame(resolver), kWallFace - kProbe, 1e-3F);
    CY_REQUIRE(world.server->destroy_body(world.wall).has_value());
    // The frame after the wall goes still applies the last answer; the one after eases, and none
    // of them pops straight back to six metres.
    f32 previous = camera.frame(resolver);
    CY_CHECK_NEAR(previous, kWallFace - kProbe, 1e-3F);
    f32 largest_step = 0.0F;
    for (int step = 0; step < 300; ++step) {
        const f32 next = camera.frame(resolver);
        CY_CHECK_GE(next, previous - 1e-5F);
        largest_step = cy::math::max(largest_step, next - previous);
        previous = next;
    }
    CY_CHECK_LT(largest_step, 0.25F);
    CY_CHECK_NEAR(previous, 6.0F, 1e-3F);
}

CY_TEST_CASE("game_backend: a probe with no radius is answered by a ray") {
    // A rig authored with a zero probe radius asks about a line, not a sphere: the camera comes in
    // to the wall's face itself.
    World world;
    Camera camera(0.0F);
    cy::game_backend::CameraCollisionResolver resolver(allocator(), *world.server, world.world);
    const physics::BodyHandle ignored[] = {world.character};
    CY_REQUIRE(resolver.set_ignored({ignored, 1}).has_value());
    (void)camera.frame(resolver);
    CY_CHECK_NEAR(resolver.stats().nearest_fraction, kWallFace / 6.0F, 1e-3F);
    CY_CHECK_NEAR(camera.frame(resolver), kWallFace, 1e-3F);
}
