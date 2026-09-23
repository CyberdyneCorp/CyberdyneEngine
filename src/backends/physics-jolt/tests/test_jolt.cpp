// The Jolt backend, through `PhysicsServer` and nothing else. Task 4.2.2.
//
// EVERY CASE HERE IS WRITTEN AGAINST THE INTERFACE. Not one line names a JPH type — the test target
// does not even link Jolt's headers, because `cy::dep::jolt` is a PRIVATE dependency of the
// backend. That is `physics`' "Backend types do not leak" checked by the build rather than by
// review: if a case here could name `JPH::Body`, the requirement would already be broken.
//
// WHAT IS ASSERTED IS WHAT THE REFERENCE BACKEND CANNOT DO, plus the behaviours both must agree on.
// Duplicating the whole of src/servers/physics/tests/ here would be duplication for its own sake;
// what earns its place is contact RESOLUTION (the reference backend has none), the shape cache over
// a real solver, and the determinism claim over the backend a game actually ships.

#include <cy/backends/physics/jolt/server.h>
#include <cy/core/determinism/hash.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/servers/physics/determinism.h>
#include <cy/test/test.h>

#include <atomic>
#include <cmath>
#include <string_view>

using namespace cy;
using namespace cy::physics;

namespace {

Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Physics);
}

struct Fixture {
    explicit Fixture(cy::jobs::JobSystem* jobs = nullptr) noexcept {
        const Expected<PhysicsServer*, Error> made = jolt::create_server(allocator(), jobs);
        CY_REQUIRE(made.has_value());
        server = *made;
        CY_REQUIRE(server->initialize().has_value());
        WorldDescription description;
        description.name = Name::intern("jolt-test");
        description.body_capacity = 256;
        description.body_pair_capacity = 1024;
        description.contact_constraint_capacity = 1024;
        const Expected<WorldHandle, Error> created = server->create_world(description);
        CY_REQUIRE(created.has_value());
        world = *created;
    }

    ~Fixture() {
        if (server != nullptr) {
            server->shutdown();
            jolt::destroy_server(server, allocator());
        }
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    [[nodiscard]] ShapeHandle box(Vec3 half_extents) const noexcept {
        ShapeDescription description;
        description.type = ShapeType::Box;
        description.half_extents = half_extents;
        const Expected<ShapeHandle, Error> shape = server->create_shape(description);
        CY_REQUIRE(shape.has_value());
        return *shape;
    }

    [[nodiscard]] ShapeHandle sphere(f32 radius) const noexcept {
        ShapeDescription description;
        description.type = ShapeType::Sphere;
        description.radius = radius;
        const Expected<ShapeHandle, Error> shape = server->create_shape(description);
        CY_REQUIRE(shape.has_value());
        return *shape;
    }

    [[nodiscard]] BodyHandle body(ShapeHandle shape, MotionType motion, Vec3 position,
                                  UserData user_data = 0, bool trigger = false) const noexcept {
        ColliderDescription collider;
        collider.shape = shape;
        collider.is_trigger = trigger;
        BodyDescription description;
        description.motion = motion;
        description.transform = Transform::from_translation(position);
        description.colliders = &collider;
        description.collider_count = 1;
        description.user_data = user_data;
        const Expected<BodyHandle, Error> made = server->create_body(world, description);
        CY_REQUIRE(made.has_value());
        return *made;
    }

    Status step(u64 tick, f32 delta = 1.0f / 60.0f) const noexcept {
        StepInput input;
        input.delta_seconds = delta;
        input.tick = tick;
        return server->step(world, input);
    }

    [[nodiscard]] Vec3 position_of(BodyHandle body_handle) const noexcept {
        const Expected<BodyState, Error> state = server->body_state(body_handle);
        CY_REQUIRE(state.has_value());
        return state->transform.translation;
    }

    PhysicsServer* server = nullptr;
    WorldHandle world;
};

struct DebugCapture final : DebugDrawSink {
    u32 lines = 0;
    u32 boxes = 0;
    u32 spheres = 0;
    u32 capsules = 0;
    u32 contacts = 0;
    u32 bounds = 0;
    u32 limits = 0;
    u32 limit_spheres = 0;
    u32 asleep_markers = 0;
    Vec3 last_sphere_center{};

    void line(Vec3, Vec3, DebugColor color) noexcept override {
        ++lines;
        if (color == DebugColor::ConstraintLimit) {
            ++limits;
        }
    }
    void box(const Aabb&, const Transform&, DebugColor color) noexcept override {
        ++boxes;
        if (color == DebugColor::Bounds) {
            ++bounds;
        }
    }
    void sphere(Vec3 center, f32, DebugColor color) noexcept override {
        ++spheres;
        last_sphere_center = center;
        if (color == DebugColor::ConstraintLimit) {
            ++limit_spheres;
        } else if (color == DebugColor::DynamicAsleep) {
            ++asleep_markers;
        }
    }
    void capsule(const Transform&, f32, f32, DebugColor) noexcept override { ++capsules; }
    void contact(Vec3, Vec3, f32) noexcept override { ++contacts; }
};

}  // namespace

CY_TEST_CASE("the Jolt backend reports itself and what it can do") {
    const Fixture fixture;
    // Compared as TEXT, not as pointers. Two identical string literals are merged at -O2 and are
    // not at -O0, so `CHECK_EQ` on the `const char*` passes in three profiles and fails in Debug —
    // measured, which is the whole reason this project builds in more than one.
    CY_CHECK(std::string_view(fixture.server->backend_name()) == jolt::kBackendName);
    CY_CHECK_FALSE(fixture.server->is_null_backend());
    const Capabilities capabilities = fixture.server->capabilities();
    // The one the reference backend cannot claim, and the reason this backend exists.
    CY_CHECK(capabilities.contact_resolution);
    CY_CHECK(capabilities.triangle_meshes);
    CY_CHECK(capabilities.convex_hulls);
    CY_CHECK(capabilities.continuous_collision);
    CY_CHECK(capabilities.constraints);
    CY_CHECK(capabilities.soft_bodies);
    CY_CHECK_EQ(capabilities.determinism, DeterminismPolicy::SamePlatformDeterministic);
    // With no engine job system given, the work runs on the calling thread and the flag says so
    // rather than claiming a bridge that is not there.
    CY_CHECK_FALSE(capabilities.uses_engine_jobs);
}

CY_TEST_CASE("Jolt cloth keeps pinned corners and advances free vertices") {
    Fixture fixture;
    const SoftBodyVertex vertices[] = {{{-1.0f, 0.0f, -1.0f}, 0.0f},
                                       {{1.0f, 0.0f, -1.0f}, 0.0f},
                                       {{1.0f, 0.0f, 1.0f}, 0.0f},
                                       {{-1.0f, 0.0f, 1.0f}, 0.0f},
                                       {{0.0f, 0.0f, 0.0f}, 1.0f}};
    const u32 triangles[] = {0, 4, 1, 1, 4, 2, 2, 4, 3, 3, 4, 0};
    SoftBodyDescription description;
    description.transform = Transform::from_translation(Vec3{0.0f, 4.0f, 0.0f});
    description.vertices = vertices;
    description.vertex_count = 5;
    description.indices = triangles;
    description.index_count = 12;
    const auto cloth = fixture.server->create_soft_body(fixture.world, description);
    CY_REQUIRE(cloth.has_value());
    const Status motion_change = fixture.server->set_body_motion_type(*cloth, MotionType::Static);
    CY_REQUIRE_FALSE(motion_change.has_value());
    CY_CHECK_EQ(motion_change.error().code, ErrorCode::Unsupported);
    const BodyHandle falling =
        fixture.body(fixture.sphere(0.2f), MotionType::Dynamic, Vec3{0.0f, 5.0f, 0.0f});
    Vec3 short_buffer[4];
    const auto short_read = fixture.server->soft_body_vertices(*cloth, Span<Vec3>(short_buffer, 4));
    CY_REQUIRE_FALSE(short_read.has_value());
    CY_CHECK_EQ(short_read.error().code, ErrorCode::BufferTooSmall);
    Vec3 initial[5];
    CY_REQUIRE(fixture.server->soft_body_vertices(*cloth, Span<Vec3>(initial, 5)).has_value());
    for (u64 tick = 0; tick < 90; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
    }
    Vec3 deformed[5];
    const auto count = fixture.server->soft_body_vertices(*cloth, Span<Vec3>(deformed, 5));
    CY_REQUIRE(count.has_value());
    CY_CHECK_EQ(*count, 5U);
    CY_CHECK_NEAR(deformed[0].y, initial[0].y, 0.01f);
    CY_CHECK_NEAR(deformed[1].y, initial[1].y, 0.01f);
    CY_CHECK_LT(deformed[4].y, initial[4].y - 0.005f);
    CY_CHECK_GT(fixture.position_of(falling).y, 3.0f);
    CY_CHECK(fixture.server->body_alive(*cloth));
    CY_REQUIRE(fixture.server->destroy_body(*cloth).has_value());
    CY_CHECK_FALSE(fixture.server->body_alive(*cloth));
    CY_CHECK_FALSE(fixture.server->soft_body_vertices(*cloth, Span<Vec3>(deformed, 5)).has_value());
}

CY_TEST_CASE("Jolt creates every declared joint type and invalidates handles with their bodies") {
    Fixture fixture;
    const ShapeHandle shape = fixture.box(Vec3{0.25f, 0.25f, 0.25f});
    const BodyHandle a = fixture.body(shape, MotionType::Dynamic, Vec3{-1.0f, 2.0f, 0.0f});
    const BodyHandle b = fixture.body(shape, MotionType::Dynamic, Vec3{1.0f, 2.0f, 0.0f});
    const ConstraintType types[] = {ConstraintType::Fixed,         ConstraintType::Point,
                                    ConstraintType::Hinge,         ConstraintType::Slider,
                                    ConstraintType::Distance,      ConstraintType::Cone,
                                    ConstraintType::SwingTwist,    ConstraintType::SixDof,
                                    ConstraintType::RackAndPinion, ConstraintType::Gear};
    ConstraintHandle handles[10];
    for (usize index = 0; index < 10; ++index) {
        ConstraintDescription description;
        description.type = types[index];
        description.body_a = a;
        description.body_b = b;
        description.frame_a.translation = Vec3{1.0f, 0.0f, 0.0f};
        description.frame_b.translation = Vec3{-1.0f, 0.0f, 0.0f};
        description.min_distance = 2.0f;
        description.max_distance = 2.0f;
        description.swing_limit_y = 0.5f;
        description.swing_limit_z = 0.5f;
        const auto made = fixture.server->create_constraint(fixture.world, description);
        CY_REQUIRE(made.has_value());
        handles[index] = *made;
        CY_CHECK(fixture.server->set_constraint_enabled(*made, false).has_value());
    }
    CY_REQUIRE(fixture.step(1).has_value());
    CY_CHECK_EQ(fixture.server->statistics(fixture.world)->constraint_count, 10U);
    CY_REQUIRE(fixture.server->destroy_body(a).has_value());
    for (const ConstraintHandle handle : handles) {
        CY_CHECK_FALSE(fixture.server->destroy_constraint(handle).has_value());
    }
    CY_CHECK_EQ(fixture.server->statistics(fixture.world)->constraint_count, 10U);
    CY_REQUIRE(fixture.step(2).has_value());
    CY_CHECK_EQ(fixture.server->statistics(fixture.world)->constraint_count, 0U);
}

CY_TEST_CASE("a breakable Jolt joint reports one measured event and stops constraining") {
    Fixture fixture;
    const ShapeHandle shape = fixture.sphere(0.25f);
    const BodyHandle ball = fixture.body(shape, MotionType::Dynamic, Vec3{0.0f, 2.0f, 0.0f});
    ConstraintDescription description;
    description.type = ConstraintType::Point;
    description.body_a = ball;
    description.frame_b.translation = Vec3{0.0f, 2.0f, 0.0f};
    description.break_force = 0.01f;
    description.user_data = 77;
    const auto made = fixture.server->create_constraint(fixture.world, description);
    CY_REQUIRE(made.has_value());
    CY_REQUIRE(fixture.server->add_impulse(ball, Vec3{10.0f, 0.0f, 0.0f}).has_value());
    CY_REQUIRE(fixture.step(1).has_value());
    const auto broken = fixture.server->broken_constraints(fixture.world);
    CY_REQUIRE(broken.has_value());
    CY_REQUIRE_EQ(broken->size(), 1U);
    CY_CHECK_EQ((*broken)[0].constraint, *made);
    CY_CHECK_EQ((*broken)[0].user_data, 77U);
    CY_CHECK_GT((*broken)[0].force, description.break_force);
    CY_REQUIRE(fixture.step(2).has_value());
    CY_CHECK_EQ(fixture.server->broken_constraints(fixture.world)->size(), 0U);
}

CY_TEST_CASE("a Jolt hinge breaks on measured torque without repeating its event") {
    Fixture fixture;
    const ShapeHandle shape = fixture.box(Vec3{0.2f, 0.2f, 0.2f});
    const BodyHandle base = fixture.body(shape, MotionType::Static, Vec3{});
    const BodyHandle driven = fixture.body(shape, MotionType::Dynamic, Vec3{});
    ConstraintDescription description;
    description.type = ConstraintType::Hinge;
    description.body_a = base;
    description.body_b = driven;
    description.limit = AxisLimit{-0.1f, 0.1f};
    description.break_torque = 0.01f;
    const auto joint = fixture.server->create_constraint(fixture.world, description);
    CY_REQUIRE(joint.has_value());
    CY_REQUIRE(fixture.server->add_angular_impulse(driven, Vec3{10.0f, 0.0f, 0.0f}).has_value());
    bool saw_break = false;
    for (u64 tick = 0; tick < 30 && !saw_break; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
        const auto broken = fixture.server->broken_constraints(fixture.world);
        CY_REQUIRE(broken.has_value());
        if (!broken->empty()) {
            CY_CHECK_EQ(broken->size(), 1U);
            CY_CHECK_EQ((*broken)[0].constraint, *joint);
            CY_CHECK_GT((*broken)[0].torque, description.break_torque);
            saw_break = true;
        }
    }
    CY_CHECK(saw_break);
    CY_REQUIRE(fixture.step(31).has_value());
    CY_CHECK_EQ(fixture.server->broken_constraints(fixture.world)->size(), 0U);
}

CY_TEST_CASE("a six-degree Jolt motor drives its configured linear axis") {
    Fixture fixture;
    const ShapeHandle shape = fixture.box(Vec3{0.2f, 0.2f, 0.2f});
    const BodyHandle base = fixture.body(shape, MotionType::Static, Vec3{0.0f, 0.0f, 0.0f});
    const BodyHandle driven = fixture.body(shape, MotionType::Dynamic, Vec3{0.0f, 0.0f, 0.0f});
    ConstraintDescription description;
    description.type = ConstraintType::SixDof;
    description.body_a = base;
    description.body_b = driven;
    description.dof_motors[1].target_velocity = 2.0f;
    description.dof_motors[1].max_force = 1000.0f;
    CY_REQUIRE(fixture.server->create_constraint(fixture.world, description).has_value());
    for (u64 tick = 0; tick < 30; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
    }
    CY_CHECK_GT(fixture.position_of(driven).y, 0.4f);
}

CY_TEST_CASE("a Jolt joint suppresses collision only while joined") {
    Fixture fixture;
    const ShapeHandle shape = fixture.sphere(0.5f);
    const BodyHandle a = fixture.body(shape, MotionType::Dynamic, Vec3{0.0f, 2.0f, 0.0f});
    const BodyHandle b = fixture.body(shape, MotionType::Dynamic, Vec3{0.5f, 2.0f, 0.0f});
    ConstraintDescription description;
    description.type = ConstraintType::Fixed;
    description.body_a = a;
    description.body_b = b;
    const auto joint = fixture.server->create_constraint(fixture.world, description);
    CY_REQUIRE(joint.has_value());
    CY_REQUIRE(fixture.step(1).has_value());
    CY_CHECK_EQ(fixture.server->events(fixture.world)->size(), 0U);
    CY_REQUIRE(fixture.server->destroy_constraint(*joint).has_value());
    CY_REQUIRE(fixture.step(2).has_value());
    CY_CHECK_GT(fixture.server->events(fixture.world)->size(), 0U);
}

CY_TEST_CASE("a Jolt hinge motor can be enabled at runtime and drives within its force cap") {
    Fixture fixture;
    const ShapeHandle shape = fixture.box(Vec3{0.2f, 0.2f, 0.2f});
    const BodyHandle base = fixture.body(shape, MotionType::Static, Vec3{0.0f, 0.0f, 0.0f});
    const BodyHandle driven = fixture.body(shape, MotionType::Dynamic, Vec3{0.0f, 0.0f, 0.0f});
    ConstraintDescription description;
    description.type = ConstraintType::Hinge;
    description.body_a = base;
    description.body_b = driven;
    const auto joint = fixture.server->create_constraint(fixture.world, description);
    CY_REQUIRE(joint.has_value());
    MotorSettings motor;
    motor.target_velocity = 3.0f;
    motor.max_force = 100.0f;
    CY_REQUIRE(fixture.server->set_constraint_motor(*joint, motor).has_value());
    for (u64 tick = 0; tick < 30; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
    }
    CY_CHECK_GT(fixture.server->body_state(driven)->angular_velocity.x, 0.5f);
}

CY_TEST_CASE("a Jolt hinge motor respects its torque cap and angular limit") {
    auto spin_after_one_step = [](f32 max_torque) noexcept {
        Fixture fixture;
        const ShapeHandle shape = fixture.box(Vec3{0.2f, 0.2f, 0.2f});
        const BodyHandle base = fixture.body(shape, MotionType::Static, Vec3{0.0f, 0.0f, 0.0f});
        const BodyHandle driven = fixture.body(shape, MotionType::Dynamic, Vec3{0.0f, 0.0f, 0.0f});
        ConstraintDescription description;
        description.type = ConstraintType::Hinge;
        description.body_a = base;
        description.body_b = driven;
        description.motor.target_velocity = 10.0f;
        description.motor.max_force = max_torque;
        CY_REQUIRE(fixture.server->create_constraint(fixture.world, description).has_value());
        CY_REQUIRE(fixture.step(1).has_value());
        return fixture.server->body_state(driven)->angular_velocity.x;
    };
    const f32 limited = spin_after_one_step(0.1f);
    const f32 strong = spin_after_one_step(100.0f);
    CY_CHECK_GT(strong, limited * 5.0f);
    CY_CHECK_LT(limited, 0.1f);

    Fixture fixture;
    const ShapeHandle shape = fixture.box(Vec3{0.2f, 0.2f, 0.2f});
    const BodyHandle base = fixture.body(shape, MotionType::Static, Vec3{0.0f, 0.0f, 0.0f});
    const BodyHandle driven = fixture.body(shape, MotionType::Dynamic, Vec3{0.0f, 0.0f, 0.0f});
    ConstraintDescription description;
    description.type = ConstraintType::Hinge;
    description.body_a = base;
    description.body_b = driven;
    description.limit = AxisLimit{-0.2f, 0.2f};
    description.motor.target_velocity = 10.0f;
    description.motor.max_force = 100.0f;
    CY_REQUIRE(fixture.server->create_constraint(fixture.world, description).has_value());
    for (u64 tick = 0; tick < 90; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
    }
    Vec3 axis;
    f32 angle = 0.0f;
    fixture.server->body_state(driven)->transform.rotation.to_axis_angle(axis, angle);
    CY_CHECK_LT(std::fabs(angle), 0.35f);
}

CY_TEST_CASE("Jolt fixed, slider and distance joints constrain different degrees of freedom") {
    {
        Fixture fixture;
        const ShapeHandle shape = fixture.sphere(0.2f);
        const BodyHandle base = fixture.body(shape, MotionType::Static, Vec3{0.0f, 0.0f, 0.0f});
        const BodyHandle driven = fixture.body(shape, MotionType::Dynamic, Vec3{1.0f, 0.0f, 0.0f});
        ConstraintDescription description;
        description.type = ConstraintType::Fixed;
        description.body_a = base;
        description.body_b = driven;
        description.frame_a.translation = Vec3{1.0f, 0.0f, 0.0f};
        CY_REQUIRE(fixture.server->create_constraint(fixture.world, description).has_value());
        for (u64 tick = 0; tick < 30; ++tick) {
            CY_REQUIRE(fixture.step(tick).has_value());
        }
        CY_CHECK_NEAR(fixture.position_of(driven).x, 1.0f, 0.05f);
        CY_CHECK_NEAR(fixture.position_of(driven).y, 0.0f, 0.05f);
    }
    {
        Fixture fixture;
        const ShapeHandle shape = fixture.sphere(0.2f);
        const BodyHandle base = fixture.body(shape, MotionType::Static, Vec3{0.0f, 0.0f, 0.0f});
        const BodyHandle driven = fixture.body(shape, MotionType::Dynamic, Vec3{0.0f, 0.0f, 0.0f});
        ConstraintDescription description;
        description.type = ConstraintType::Slider;
        description.body_a = base;
        description.body_b = driven;
        description.limit = AxisLimit{-0.5f, 0.5f};
        CY_REQUIRE(fixture.server->create_constraint(fixture.world, description).has_value());
        CY_REQUIRE(fixture.server->add_impulse(driven, Vec3{100.0f, 0.0f, 0.0f}).has_value());
        for (u64 tick = 0; tick < 30; ++tick) {
            CY_REQUIRE(fixture.step(tick).has_value());
        }
        CY_CHECK_GT(fixture.position_of(driven).x, 0.2f);
        CY_CHECK_LT(fixture.position_of(driven).x, 0.6f);
        CY_CHECK_NEAR(fixture.position_of(driven).y, 0.0f, 0.05f);
    }
    {
        Fixture fixture;
        const ShapeHandle shape = fixture.sphere(0.2f);
        const BodyHandle base = fixture.body(shape, MotionType::Static, Vec3{0.0f, 0.0f, 0.0f});
        const BodyHandle driven = fixture.body(shape, MotionType::Dynamic, Vec3{2.0f, 0.0f, 0.0f});
        ConstraintDescription description;
        description.type = ConstraintType::Distance;
        description.body_a = base;
        description.body_b = driven;
        description.min_distance = 2.0f;
        description.max_distance = 2.0f;
        CY_REQUIRE(fixture.server->create_constraint(fixture.world, description).has_value());
        for (u64 tick = 0; tick < 60; ++tick) {
            CY_REQUIRE(fixture.step(tick).has_value());
        }
        CY_CHECK_NEAR(length(fixture.position_of(driven)), 2.0f, 0.05f);
    }
}

CY_TEST_CASE("a Jolt point joint swings around its fixed anchor") {
    Fixture fixture;
    const ShapeHandle shape = fixture.sphere(0.2f);
    const BodyHandle base = fixture.body(shape, MotionType::Static, Vec3{0.0f, 0.0f, 0.0f});
    const BodyHandle driven = fixture.body(shape, MotionType::Dynamic, Vec3{1.0f, 0.0f, 0.0f});
    ConstraintDescription description;
    description.type = ConstraintType::Point;
    description.body_a = base;
    description.body_b = driven;
    description.frame_b.translation = Vec3{-1.0f, 0.0f, 0.0f};
    CY_REQUIRE(fixture.server->create_constraint(fixture.world, description).has_value());
    for (u64 tick = 0; tick < 60; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
    }
    CY_CHECK_LT(fixture.position_of(driven).y, -0.2f);
    CY_CHECK_NEAR(length(fixture.position_of(driven)), 1.0f, 0.1f);
}

CY_TEST_CASE("a Jolt six-degree position spring pulls toward its per-axis target") {
    Fixture fixture;
    const ShapeHandle shape = fixture.sphere(0.2f);
    const BodyHandle base = fixture.body(shape, MotionType::Static, Vec3{0.0f, 0.0f, 0.0f});
    const BodyHandle driven = fixture.body(shape, MotionType::Dynamic, Vec3{0.0f, 0.0f, 0.0f});
    ConstraintDescription description;
    description.type = ConstraintType::SixDof;
    description.body_a = base;
    description.body_b = driven;
    description.dof_motors[1].position_driven = true;
    description.dof_motors[1].target_position = 1.0f;
    description.dof_motors[1].max_force = 1000.0f;
    description.dof_motors[1].spring_frequency = 2.0f;
    description.dof_motors[1].spring_damping = 1.0f;
    CY_REQUIRE(fixture.server->create_constraint(fixture.world, description).has_value());
    for (u64 tick = 0; tick < 60; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
    }
    CY_CHECK_GT(fixture.position_of(driven).y, 0.3f);
}

CY_TEST_CASE("a Jolt six-degree joint enforces linear and angular axis limits") {
    Fixture fixture;
    const ShapeHandle shape = fixture.box(Vec3{0.2f, 0.2f, 0.2f});
    const BodyHandle base = fixture.body(shape, MotionType::Static, Vec3{});
    const BodyHandle driven = fixture.body(shape, MotionType::Dynamic, Vec3{});
    ConstraintDescription description;
    description.type = ConstraintType::SixDof;
    description.body_a = base;
    description.body_b = driven;
    description.dof_limits[0] = AxisLimit{-0.5f, 0.5f};
    description.dof_limits[3] = AxisLimit{-0.2f, 0.2f};
    CY_REQUIRE(fixture.server->create_constraint(fixture.world, description).has_value());
    CY_REQUIRE(fixture.server->add_impulse(driven, Vec3{100.0f, 0.0f, 0.0f}).has_value());
    CY_REQUIRE(fixture.server->add_angular_impulse(driven, Vec3{5.0f, 0.0f, 0.0f}).has_value());
    for (u64 tick = 0; tick < 30; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
    }
    const auto state = fixture.server->body_state(driven);
    CY_REQUIRE(state.has_value());
    CY_CHECK_LT(std::fabs(state->transform.translation.x), 0.6f);
    CY_CHECK_LT(std::fabs(state->transform.rotation.to_euler_yxz().x), 0.35f);
}

CY_TEST_CASE("Jolt cone and swing-twist joints enforce their swing limits") {
    for (const ConstraintType type : {ConstraintType::Cone, ConstraintType::SwingTwist}) {
        Fixture fixture;
        const ShapeHandle shape = fixture.box(Vec3{0.2f, 0.2f, 0.2f});
        const BodyHandle base = fixture.body(shape, MotionType::Static, Vec3{});
        const BodyHandle driven = fixture.body(shape, MotionType::Dynamic, Vec3{});
        ConstraintDescription description;
        description.type = type;
        description.body_a = base;
        description.body_b = driven;
        description.swing_limit_y = 0.2f;
        description.swing_limit_z = 0.2f;
        description.twist_limit = AxisLimit{-0.2f, 0.2f};
        CY_REQUIRE(fixture.server->create_constraint(fixture.world, description).has_value());
        CY_REQUIRE(fixture.server->add_angular_impulse(driven, Vec3{0.0f, 5.0f, 0.0f}).has_value());
        for (u64 tick = 0; tick < 30; ++tick) {
            CY_REQUIRE(fixture.step(tick).has_value());
        }
        const Vec3 twist_axis = fixture.server->body_state(driven)->transform.right();
        CY_CHECK_GT(twist_axis.x, std::cos(0.4f));
    }
}

CY_TEST_CASE("a Jolt swing-twist joint enforces its twist limit") {
    Fixture fixture;
    const ShapeHandle shape = fixture.box(Vec3{0.2f, 0.2f, 0.2f});
    const BodyHandle base = fixture.body(shape, MotionType::Static, Vec3{});
    const BodyHandle driven = fixture.body(shape, MotionType::Dynamic, Vec3{});
    ConstraintDescription description;
    description.type = ConstraintType::SwingTwist;
    description.body_a = base;
    description.body_b = driven;
    description.swing_limit_y = 0.5f;
    description.swing_limit_z = 0.5f;
    description.twist_limit = AxisLimit{-0.2f, 0.2f};
    CY_REQUIRE(fixture.server->create_constraint(fixture.world, description).has_value());
    CY_REQUIRE(fixture.server->add_angular_impulse(driven, Vec3{5.0f, 0.0f, 0.0f}).has_value());
    for (u64 tick = 0; tick < 30; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
    }
    CY_CHECK_LT(std::fabs(fixture.server->body_state(driven)->transform.rotation.to_euler_yxz().x),
                0.35f);
}

CY_TEST_CASE("Jolt gears couple opposite angular speeds at the configured ratio") {
    Fixture fixture;
    const ShapeHandle shape = fixture.sphere(0.2f);
    const BodyHandle first = fixture.body(shape, MotionType::Dynamic, Vec3{-1.0f, 1.0f, 0.0f});
    const BodyHandle second = fixture.body(shape, MotionType::Dynamic, Vec3{1.0f, 1.0f, 0.0f});
    for (const BodyHandle body : {first, second}) {
        ConstraintDescription hinge;
        hinge.type = ConstraintType::Hinge;
        hinge.body_a = body;
        hinge.frame_b.translation = fixture.position_of(body);
        CY_REQUIRE(fixture.server->create_constraint(fixture.world, hinge).has_value());
    }
    ConstraintDescription gears;
    gears.type = ConstraintType::Gear;
    gears.body_a = first;
    gears.body_b = second;
    gears.ratio = 2.0f;
    CY_REQUIRE(fixture.server->create_constraint(fixture.world, gears).has_value());
    CY_REQUIRE(
        fixture.server->set_body_velocity(first, Vec3{}, Vec3{4.0f, 0.0f, 0.0f}).has_value());
    for (u64 tick = 0; tick < 5; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
    }
    const f32 first_speed = fixture.server->body_state(first)->angular_velocity.x;
    const f32 second_speed = fixture.server->body_state(second)->angular_velocity.x;
    CY_CHECK_GT(first_speed, 0.1f);
    CY_CHECK_LT(second_speed, -0.1f);
    CY_CHECK_NEAR(first_speed + 2.0f * second_speed, 0.0f, 0.2f);
}

CY_TEST_CASE("Jolt rack and pinion transfers gear rotation into rack travel") {
    Fixture fixture;
    const ShapeHandle shape = fixture.sphere(0.2f);
    const BodyHandle pinion = fixture.body(shape, MotionType::Dynamic, Vec3{-1.0f, 1.0f, 0.0f});
    const BodyHandle rack = fixture.body(shape, MotionType::Dynamic, Vec3{1.0f, 1.0f, 0.0f});
    ConstraintDescription hinge;
    hinge.type = ConstraintType::Hinge;
    hinge.body_a = pinion;
    hinge.frame_b.translation = fixture.position_of(pinion);
    CY_REQUIRE(fixture.server->create_constraint(fixture.world, hinge).has_value());
    ConstraintDescription slider;
    slider.type = ConstraintType::Slider;
    slider.body_a = rack;
    slider.frame_b.translation = fixture.position_of(rack);
    CY_REQUIRE(fixture.server->create_constraint(fixture.world, slider).has_value());
    ConstraintDescription coupling;
    coupling.type = ConstraintType::RackAndPinion;
    coupling.body_a = pinion;
    coupling.body_b = rack;
    coupling.ratio = 2.0f;
    CY_REQUIRE(fixture.server->create_constraint(fixture.world, coupling).has_value());
    CY_REQUIRE(
        fixture.server->set_body_velocity(pinion, Vec3{}, Vec3{4.0f, 0.0f, 0.0f}).has_value());
    for (u64 tick = 0; tick < 5; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
    }
    const f32 spin = fixture.server->body_state(pinion)->angular_velocity.x;
    const f32 travel = fixture.server->body_state(rack)->linear_velocity.x;
    CY_CHECK_GT(std::fabs(travel), 0.1f);
    CY_CHECK_NEAR(std::fabs(spin), 2.0f * std::fabs(travel), 0.2f);
}

CY_TEST_CASE(
    "Jolt debug flags emit distinct solver-space shape, sleep, velocity and bounds views") {
    Fixture fixture;
    const BodyHandle ball =
        fixture.body(fixture.sphere(0.5f), MotionType::Dynamic, Vec3{2.0f, 3.0f, 4.0f});
    DebugCapture capture;
    CY_REQUIRE(
        fixture.server->debug_draw(fixture.world, DebugDrawFlags::Colliders, capture).has_value());
    CY_CHECK_EQ(capture.spheres, 1U);
    CY_CHECK_EQ(capture.boxes, 0U);
    CY_CHECK_NEAR(capture.last_sphere_center.x, fixture.position_of(ball).x, 0.001f);

    DebugCapture bounds_capture;
    CY_REQUIRE(
        fixture.server->debug_draw(fixture.world, DebugDrawFlags::BroadPhaseBounds, bounds_capture)
            .has_value());
    CY_CHECK_EQ(bounds_capture.bounds, 1U);
    CY_CHECK_EQ(bounds_capture.spheres, 0U);

    DebugCapture state_capture;
    CY_REQUIRE(fixture.server
                   ->debug_draw(fixture.world,
                                DebugDrawFlags::SleepState | DebugDrawFlags::CentersOfMass |
                                    DebugDrawFlags::Velocities,
                                state_capture)
                   .has_value());
    CY_CHECK_EQ(state_capture.spheres, 2U);
    CY_CHECK_EQ(state_capture.lines, 2U);
}

CY_TEST_CASE("Jolt collider debug emits hull and mesh triangles at solver transforms") {
    Fixture fixture;
    const Vec3 hull_points[] = {
        {-0.5f, 0.0f, -0.5f}, {0.5f, 0.0f, -0.5f}, {0.0f, 0.0f, 0.5f}, {0.0f, 1.0f, 0.0f}};
    ShapeDescription hull;
    hull.type = ShapeType::ConvexHull;
    hull.points = hull_points;
    hull.point_count = 4;
    const auto hull_shape = fixture.server->create_shape(hull);
    CY_REQUIRE(hull_shape.has_value());
    (void)fixture.body(*hull_shape, MotionType::Dynamic, Vec3{2.0f, 3.0f, 0.0f});

    const Vec3 vertices[] = {{-1.0f, 0.0f, -1.0f}, {1.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f}};
    const u32 indices[] = {0, 1, 2};
    ShapeDescription mesh;
    mesh.type = ShapeType::TriangleMesh;
    mesh.vertices = vertices;
    mesh.vertex_count = 3;
    mesh.indices = indices;
    mesh.index_count = 3;
    const auto mesh_shape = fixture.server->create_shape(mesh);
    CY_REQUIRE(mesh_shape.has_value());
    (void)fixture.body(*mesh_shape, MotionType::Static, Vec3{0.0f, -1.0f, 0.0f});

    DebugCapture capture;
    CY_REQUIRE(
        fixture.server->debug_draw(fixture.world, DebugDrawFlags::Colliders, capture).has_value());
    CY_CHECK_GT(capture.lines, 3U);
    CY_CHECK_EQ(capture.boxes, 0U);
}

CY_TEST_CASE("Jolt contact and sleep debug flags emit solver events and sleep state") {
    Fixture fixture;
    (void)fixture.body(fixture.box(Vec3{5.0f, 0.5f, 5.0f}), MotionType::Static,
                       Vec3{0.0f, -0.5f, 0.0f});
    (void)fixture.body(fixture.sphere(0.5f), MotionType::Dynamic, Vec3{0.0f, 2.0f, 0.0f});
    bool saw_contact = false;
    for (u64 tick = 0; tick < 180; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
        DebugCapture capture;
        CY_REQUIRE(fixture.server->debug_draw(fixture.world, DebugDrawFlags::Contacts, capture)
                       .has_value());
        saw_contact |= capture.contacts > 0;
    }
    CY_CHECK(saw_contact);
    DebugCapture sleeping;
    CY_REQUIRE(fixture.server->debug_draw(fixture.world, DebugDrawFlags::SleepState, sleeping)
                   .has_value());
    CY_CHECK_GT(sleeping.asleep_markers, 0U);
}

CY_TEST_CASE("Jolt constraint debug draws anchors and limit rays") {
    Fixture fixture;
    const ShapeHandle shape = fixture.sphere(0.2f);
    const BodyHandle base = fixture.body(shape, MotionType::Static, Vec3{0.0f, 0.0f, 0.0f});
    const BodyHandle driven = fixture.body(shape, MotionType::Dynamic, Vec3{0.0f, 0.0f, 0.0f});
    ConstraintDescription description;
    description.type = ConstraintType::Hinge;
    description.body_a = base;
    description.body_b = driven;
    description.limit.min = -0.5f;
    description.limit.max = 0.5f;
    CY_REQUIRE(fixture.server->create_constraint(fixture.world, description).has_value());
    DebugCapture capture;
    CY_REQUIRE(fixture.server->debug_draw(fixture.world, DebugDrawFlags::Constraints, capture)
                   .has_value());
    CY_CHECK_EQ(capture.spheres, 2U);
    CY_CHECK_EQ(capture.limits, 2U);
}

CY_TEST_CASE("Jolt constraint debug covers distance, cone, twist and six-axis limits") {
    Fixture fixture;
    const ShapeHandle shape = fixture.sphere(0.2f);
    const BodyHandle base = fixture.body(shape, MotionType::Static, Vec3{0.0f, 0.0f, 0.0f});
    const BodyHandle driven = fixture.body(shape, MotionType::Dynamic, Vec3{0.0f, 0.0f, 0.0f});
    ConstraintDescription description;
    description.body_a = base;
    description.body_b = driven;
    description.type = ConstraintType::Distance;
    description.min_distance = 0.25f;
    description.max_distance = 1.0f;
    CY_REQUIRE(fixture.server->create_constraint(fixture.world, description).has_value());
    description.type = ConstraintType::Cone;
    description.swing_limit_y = 0.3f;
    description.swing_limit_z = 0.4f;
    CY_REQUIRE(fixture.server->create_constraint(fixture.world, description).has_value());
    description.type = ConstraintType::SwingTwist;
    description.twist_limit = AxisLimit{-0.2f, 0.2f};
    CY_REQUIRE(fixture.server->create_constraint(fixture.world, description).has_value());
    description.type = ConstraintType::SixDof;
    description.dof_limits[0] = AxisLimit{-1.0f, 1.0f};
    description.dof_limits[5] = AxisLimit{-0.5f, 0.5f};
    CY_REQUIRE(fixture.server->create_constraint(fixture.world, description).has_value());
    DebugCapture capture;
    CY_REQUIRE(fixture.server->debug_draw(fixture.world, DebugDrawFlags::Constraints, capture)
                   .has_value());
    CY_CHECK_EQ(capture.limit_spheres, 2U);
    CY_CHECK_EQ(capture.limits, 13U);
}

CY_TEST_CASE("Jolt step statistics measure broad, narrow and solve job costs") {
    Fixture fixture;
    (void)fixture.body(fixture.box(Vec3{10.0f, 0.5f, 10.0f}), MotionType::Static,
                       Vec3{0.0f, -0.5f, 0.0f});
    const ShapeHandle shape = fixture.sphere(0.5f);
    for (u32 index = 0; index < 12; ++index) {
        (void)fixture.body(shape, MotionType::Dynamic,
                           Vec3{static_cast<f32>(index) * 0.8f, 0.6f, 0.0f});
    }
    CY_REQUIRE(fixture.step(1).has_value());
    const auto stats = fixture.server->statistics(fixture.world);
    CY_REQUIRE(stats.has_value());
    CY_CHECK_GT(stats->broad_phase_ns, 0);
    CY_CHECK_GT(stats->narrow_phase_ns, 0);
    CY_CHECK_GT(stats->solve_ns, 0);
    CY_CHECK_GT(stats->total_ns, 0);
}

CY_TEST_CASE("a dense contact scene exposes narrow-phase cost in step diagnostics") {
    Fixture fixture;
    (void)fixture.body(fixture.box(Vec3{20.0f, 0.5f, 20.0f}), MotionType::Static,
                       Vec3{0.0f, -0.5f, 0.0f});
    const ShapeHandle sphere = fixture.sphere(0.5f);
    for (u32 x = 0; x < 12; ++x) {
        for (u32 z = 0; z < 12; ++z) {
            (void)fixture.body(sphere, MotionType::Dynamic,
                               Vec3{static_cast<f32>(x) * 0.8f, 0.5f, static_cast<f32>(z) * 0.8f});
        }
    }
    Nanoseconds broad = 0;
    Nanoseconds narrow = 0;
    Nanoseconds solve = 0;
    for (u64 tick = 0; tick < 12; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
        const auto stats = fixture.server->statistics(fixture.world);
        CY_REQUIRE(stats.has_value());
        broad += stats->broad_phase_ns;
        narrow += stats->narrow_phase_ns;
        solve += stats->solve_ns;
    }
    CY_CHECK_GT(narrow, broad);
    CY_CHECK_GT(solve, 0);
}

CY_TEST_CASE("Jolt island count joins active bodies by constraints and contacts") {
    Fixture fixture;
    const ShapeHandle shape = fixture.sphere(0.25f);
    const BodyHandle a = fixture.body(shape, MotionType::Dynamic, Vec3{-3.0f, 2.0f, 0.0f});
    const BodyHandle b = fixture.body(shape, MotionType::Dynamic, Vec3{3.0f, 2.0f, 0.0f});
    CY_REQUIRE(fixture.step(1).has_value());
    CY_CHECK_EQ(fixture.server->statistics(fixture.world)->island_count, 2U);

    ConstraintDescription description;
    description.type = ConstraintType::Distance;
    description.body_a = a;
    description.body_b = b;
    description.min_distance = 6.0f;
    description.max_distance = 6.0f;
    const auto joint = fixture.server->create_constraint(fixture.world, description);
    CY_REQUIRE(joint.has_value());
    CY_REQUIRE(fixture.step(2).has_value());
    CY_CHECK_EQ(fixture.server->statistics(fixture.world)->island_count, 1U);

    CY_REQUIRE(fixture.server->destroy_constraint(*joint).has_value());
    CY_REQUIRE(fixture.step(3).has_value());
    CY_CHECK_EQ(fixture.server->statistics(fixture.world)->island_count, 2U);

    const BodyHandle c = fixture.body(shape, MotionType::Dynamic, Vec3{0.0f, 2.0f, 0.0f});
    const BodyHandle d = fixture.body(shape, MotionType::Dynamic, Vec3{0.3f, 2.0f, 0.0f});
    CY_REQUIRE(fixture.step(4).has_value());
    CY_CHECK_EQ(fixture.server->statistics(fixture.world)->island_count, 3U);
    (void)c;
    (void)d;
}

CY_TEST_CASE("a dynamic body falls onto static geometry and stops on it") {
    // THE CASE THE REFERENCE BACKEND CANNOT PASS. It detects the contact and reports it; it does
    // not resolve it, so its sphere would fall straight through. This is what `contact_resolution`
    // means.
    const Fixture fixture;
    (void)fixture.body(fixture.box(Vec3{10.0f, 0.5f, 10.0f}), MotionType::Static,
                       Vec3{0.0f, -0.5f, 0.0f});
    const BodyHandle ball =
        fixture.body(fixture.sphere(0.5f), MotionType::Dynamic, Vec3{0.0f, 4.0f, 0.0f});

    for (u64 tick = 0; tick < 180; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
    }
    const Vec3 resting = fixture.position_of(ball);
    // On the floor, not through it: the sphere's centre sits one radius above y = 0, within the
    // penetration slop.
    CY_CHECK_NEAR(resting.y, 0.5f, 0.05f);
    CY_CHECK_GT(resting.y, 0.0f);
}

CY_TEST_CASE("a resting body goes to sleep and an impulse wakes it") {
    const Fixture fixture;
    (void)fixture.body(fixture.box(Vec3{10.0f, 0.5f, 10.0f}), MotionType::Static,
                       Vec3{0.0f, -0.5f, 0.0f});
    const BodyHandle ball =
        fixture.body(fixture.sphere(0.5f), MotionType::Dynamic, Vec3{0.0f, 0.6f, 0.0f});
    for (u64 tick = 0; tick < 180; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
    }
    CY_CHECK(fixture.server->body_state(ball)->asleep);
    CY_REQUIRE(fixture.server->add_impulse(ball, Vec3{0.0f, 20.0f, 0.0f}).has_value());
    CY_REQUIRE(fixture.step(200).has_value());
    CY_CHECK_FALSE(fixture.server->body_state(ball)->asleep);
}

CY_TEST_CASE("collision events arrive with the pair, the phase and a contact point") {
    const Fixture fixture;
    const BodyHandle floor = fixture.body(fixture.box(Vec3{10.0f, 0.5f, 10.0f}), MotionType::Static,
                                          Vec3{0.0f, -0.5f, 0.0f}, 1);
    const BodyHandle ball =
        fixture.body(fixture.sphere(0.5f), MotionType::Dynamic, Vec3{0.0f, 2.0f, 0.0f}, 2);

    bool saw_enter = false;
    for (u64 tick = 0; tick < 120 && !saw_enter; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
        const Expected<Span<const ContactEvent>, Error> events =
            fixture.server->events(fixture.world);
        CY_REQUIRE(events.has_value());
        for (usize index = 0; index < events->size(); ++index) {
            const ContactEvent& event = (*events)[index];
            if (event.phase != ContactPhase::Enter) {
                continue;
            }
            saw_enter = true;
            CY_CHECK_LT(event.a.bits(), event.b.bits());
            CY_CHECK_EQ(event.user_data_a + event.user_data_b, 3U);
            CY_CHECK_EQ(event.point_count, 1U);
            CY_CHECK_FALSE(event.trigger);
            // The estimate, not zero: a ball hitting a floor at a few metres per second carries a
            // real impulse, and a backend reporting zero would make every impact silent.
            CY_CHECK_GT(event.total_impulse, 0.0f);
        }
    }
    CY_CHECK(saw_enter);
    CY_CHECK_FALSE(floor.is_null());
    CY_CHECK_FALSE(ball.is_null());
}

CY_TEST_CASE("a trigger reports overlap and does not stop the body") {
    const Fixture fixture;
    (void)fixture.body(fixture.box(Vec3{2.0f, 0.5f, 2.0f}), MotionType::Static,
                       Vec3{0.0f, 0.0f, 0.0f}, 1, true);
    const BodyHandle ball =
        fixture.body(fixture.sphere(0.25f), MotionType::Dynamic, Vec3{0.0f, 3.0f, 0.0f}, 2);

    bool saw_trigger = false;
    for (u64 tick = 0; tick < 120; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
        const Expected<Span<const ContactEvent>, Error> events =
            fixture.server->events(fixture.world);
        CY_REQUIRE(events.has_value());
        for (usize index = 0; index < events->size(); ++index) {
            saw_trigger = saw_trigger || (*events)[index].trigger;
        }
    }
    CY_CHECK(saw_trigger);
    // And it fell straight through, which is what makes it a sensor rather than a floor.
    CY_CHECK_LT(fixture.position_of(ball).y, -1.0f);
}

CY_TEST_CASE("a body whose colliders mix triggers and solids is rejected") {
    // Jolt's sensor flag is per body — see jolt_server.cpp's header, mismatch 2. Guessing which
    // half wins is how a checkpoint volume becomes a wall, so the pairing is refused instead.
    const Fixture fixture;
    ColliderDescription colliders[2];
    colliders[0].shape = fixture.box(Vec3{1.0f, 1.0f, 1.0f});
    colliders[1].shape = fixture.sphere(0.5f);
    colliders[1].local = Transform::from_translation(Vec3{2.0f, 0.0f, 0.0f});
    colliders[1].is_trigger = true;

    BodyDescription description;
    description.motion = MotionType::Static;
    description.colliders = colliders;
    description.collider_count = 2;
    const Expected<BodyHandle, Error> body =
        fixture.server->create_body(fixture.world, description);
    CY_REQUIRE_FALSE(body.has_value());
    CY_CHECK_EQ(body.error().code, ErrorCode::Unsupported);
}

CY_TEST_CASE("one thousand identical box colliders create one Jolt shape") {
    // `physics` — "Shape sharing", over the backend the requirement names. The key is computed in
    // cy_physics, so this is the same property the reference backend's suite asserts, measured on
    // the other implementation.
    const Fixture fixture;
    ShapeDescription description;
    description.type = ShapeType::Box;
    description.half_extents = Vec3{0.5f, 0.5f, 0.5f};
    ShapeHandle first;
    for (u32 index = 0; index < 1000; ++index) {
        const Expected<ShapeHandle, Error> shape = fixture.server->create_shape(description);
        CY_REQUIRE(shape.has_value());
        if (index == 0) {
            first = *shape;
        }
        CY_CHECK_EQ(shape->bits(), first.bits());
    }
    const Expected<ShapeStatistics, Error> statistics = fixture.server->shape_statistics();
    CY_REQUIRE(statistics.has_value());
    CY_CHECK_EQ(statistics->unique_shapes, 1U);
    CY_CHECK_EQ(statistics->cache_hits, 999U);
}

CY_TEST_CASE("a raycast excludes its own body and reports the surface it hit") {
    const Fixture fixture;
    const BodyHandle floor = fixture.body(fixture.box(Vec3{10.0f, 0.5f, 10.0f}), MotionType::Static,
                                          Vec3{0.0f, -0.5f, 0.0f}, 99);
    const BodyHandle self =
        fixture.body(fixture.sphere(0.5f), MotionType::Kinematic, Vec3{0.0f, 2.0f, 0.0f});
    CY_REQUIRE(fixture.step(0).has_value());

    RayCastInput ray;
    ray.origin = Vec3{0.0f, 2.0f, 0.0f};
    ray.direction = Vec3{0.0f, -1.0f, 0.0f};
    ray.max_distance = 10.0f;
    QueryFilter filter;
    filter.ignore = &self;
    filter.ignore_count = 1;
    const Expected<RayCastHit, Error> hit = fixture.server->raycast(fixture.world, ray, filter);
    CY_REQUIRE(hit.has_value());
    CY_CHECK_EQ(hit->body.bits(), floor.bits());
    CY_CHECK_EQ(hit->user_data, 99U);
    CY_CHECK_NEAR(hit->distance, 2.0f, 0.05f);
    CY_CHECK_NEAR(hit->normal.y, 1.0f, 0.01f);
}

CY_TEST_CASE("a shape cast reports the distance to first touch and a separating normal") {
    const Fixture fixture;
    (void)fixture.body(fixture.box(Vec3{10.0f, 0.5f, 10.0f}), MotionType::Static,
                       Vec3{0.0f, -0.5f, 0.0f});
    CY_REQUIRE(fixture.step(0).has_value());

    ShapeCastInput cast;
    cast.shape = fixture.sphere(0.25f);
    cast.start = Transform::from_translation(Vec3{0.0f, 5.0f, 0.0f});
    cast.direction = Vec3{0.0f, -1.0f, 0.0f};
    cast.max_distance = 10.0f;
    const Expected<ShapeCastHit, Error> hit =
        fixture.server->shape_cast(fixture.world, cast, QueryFilter{});
    CY_REQUIRE(hit.has_value());
    CY_REQUIRE_FALSE(hit->body.is_null());
    CY_CHECK_NEAR(hit->distance, 4.75f, 0.05f);
    CY_CHECK_NEAR(hit->normal.y, 1.0f, 0.05f);
}

CY_TEST_CASE("overlap and closest point answer about the bodies near a shape") {
    const Fixture fixture;
    const BodyHandle block =
        fixture.body(fixture.box(Vec3{1.0f, 1.0f, 1.0f}), MotionType::Static, Vec3{}, 5);
    CY_REQUIRE(fixture.step(0).has_value());

    OverlapInput input;
    input.shape = fixture.sphere(0.5f);
    input.transform = Transform::from_translation(Vec3{0.5f, 0.0f, 0.0f});
    OverlapHit hits[4];
    const Expected<u32, Error> count =
        fixture.server->overlap(fixture.world, input, QueryFilter{}, Span<OverlapHit>(hits, 4));
    CY_REQUIRE(count.has_value());
    CY_CHECK_EQ(*count, 1U);
    CY_CHECK_EQ(hits[0].body.bits(), block.bits());
    CY_CHECK_EQ(hits[0].user_data, 5U);

    ClosestPointInput closest_input;
    closest_input.point = Vec3{5.0f, 0.0f, 0.0f};
    const Expected<ClosestPoint, Error> closest =
        fixture.server->closest_point(fixture.world, closest_input, QueryFilter{});
    CY_REQUIRE(closest.has_value());
    CY_CHECK_EQ(closest->body.bits(), block.bits());
    CY_CHECK_NEAR(closest->distance, 4.0f, 0.1f);
}

CY_TEST_CASE("a 2D body cannot leave the plane, and Jolt is the one enforcing it") {
    // `physics` — "Constraint is enforced", over the real solver: the degrees of freedom are locked
    // in the body's creation settings, so the integrator never produces out-of-plane motion. A
    // fix-up after the step would leave the velocity behind and the body would drift.
    const Fixture fixture;
    (void)fixture.body(fixture.box(Vec3{10.0f, 0.5f, 10.0f}), MotionType::Static,
                       Vec3{0.0f, -0.5f, 0.0f});
    ColliderDescription collider;
    collider.shape = fixture.sphere(0.5f);
    BodyDescription description;
    description.motion = MotionType::Dynamic;
    description.transform = Transform::from_translation(Vec3{0.0f, 3.0f, 0.0f});
    description.locked_axes = kLockPlaneXY;
    description.colliders = &collider;
    description.collider_count = 1;
    const Expected<BodyHandle, Error> body =
        fixture.server->create_body(fixture.world, description);
    CY_REQUIRE(body.has_value());

    CY_REQUIRE(fixture.server->add_impulse(*body, Vec3{0.0f, 0.0f, 50.0f}).has_value());
    for (u64 tick = 0; tick < 120; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
    }
    const Expected<BodyState, Error> state = fixture.server->body_state(*body);
    CY_REQUIRE(state.has_value());
    CY_CHECK_NEAR(state->transform.translation.z, 0.0f, 1e-4f);
    CY_CHECK_NEAR(state->linear_velocity.z, 0.0f, 1e-4f);
}

CY_TEST_CASE("two runs of the same scene on Jolt produce identical state hashes") {
    // `physics` — "Replay reproduces a session", on the backend a game ships. Jolt is built with
    // CROSS_PLATFORM_DETERMINISTIC, which fixes the floating-point mode; the engine claims the
    // same-platform half only, and this is the measurement behind that claim.
    const auto run = [](DeterminismProbe& probe) {
        const Fixture fixture;
        (void)fixture.body(fixture.box(Vec3{10.0f, 0.5f, 10.0f}), MotionType::Static,
                           Vec3{0.0f, -0.5f, 0.0f});
        const ShapeHandle sphere = fixture.sphere(0.4f);
        const ShapeHandle box = fixture.box(Vec3{0.4f, 0.4f, 0.4f});
        for (u32 index = 0; index < 8; ++index) {
            const BodyHandle body = fixture.body(
                (index % 2) == 0 ? sphere : box, MotionType::Dynamic,
                Vec3{(static_cast<f32>(index) * 0.35f) - 1.2f,
                     1.0f + (static_cast<f32>(index) * 0.9f), static_cast<f32>(index) * 0.11f},
                index + 1);
            CY_REQUIRE(fixture.server
                           ->set_body_velocity(body,
                                               Vec3{static_cast<f32>(index) * 0.2f, 0.0f, 0.3f},
                                               Vec3{0.2f, static_cast<f32>(index), 0.1f})
                           .has_value());
        }
        const SoftBodyVertex cloth_vertices[] = {{{-1.0f, 0.0f, -1.0f}, 0.0f},
                                                 {{1.0f, 0.0f, -1.0f}, 0.0f},
                                                 {{1.0f, 0.0f, 1.0f}, 0.0f},
                                                 {{-1.0f, 0.0f, 1.0f}, 0.0f},
                                                 {{0.0f, 0.0f, 0.0f}, 1.0f}};
        const u32 cloth_triangles[] = {0, 4, 1, 1, 4, 2, 2, 4, 3, 3, 4, 0};
        SoftBodyDescription cloth;
        cloth.transform = Transform::from_translation(Vec3{0.0f, 5.0f, 0.0f});
        cloth.vertices = cloth_vertices;
        cloth.vertex_count = 5;
        cloth.indices = cloth_triangles;
        cloth.index_count = 12;
        CY_REQUIRE(fixture.server->create_soft_body(fixture.world, cloth).has_value());
        for (u32 tick = 0; tick < 90; ++tick) {
            CY_REQUIRE(fixture.step(tick).has_value());
            CY_REQUIRE(probe.record(*fixture.server, fixture.world, tick).has_value());
        }
    };

    DeterminismProbe first(allocator(), 128);
    DeterminismProbe second(allocator(), 128);
    run(first);
    run(second);
    const PhysicsDivergence divergence = DeterminismProbe::compare(first, second);
    CY_CHECK_FALSE(divergence.diverged);
    // Non-zero, so the comparison is over something: two empty trees also agree.
    CY_CHECK_NE(first.hash_at(89), 0U);
    CY_CHECK_EQ(first.hash_at(89), second.hash_at(89));
}

CY_TEST_CASE("Jolt's internal parallelism runs on engine job workers when one is given") {
    // `physics` — "One job system": "its internal parallelism SHALL run on engine job workers, so
    // physics and other work share one thread pool and one scheduler". The capability flag is the
    // observable half; the step running correctly through the bridge is the other.
    cy::jobs::JobSystem jobs;
    cy::jobs::JobSystemConfig config;
    config.worker_count = 2;
    CY_REQUIRE(jobs.start(config).has_value());

    {
        const Fixture fixture(&jobs);
        CY_CHECK(fixture.server->capabilities().uses_engine_jobs);
        (void)fixture.body(fixture.box(Vec3{10.0f, 0.5f, 10.0f}), MotionType::Static,
                           Vec3{0.0f, -0.5f, 0.0f});
        const ShapeHandle sphere = fixture.sphere(0.3f);
        for (u32 index = 0; index < 16; ++index) {
            (void)fixture.body(sphere, MotionType::Dynamic,
                               Vec3{(static_cast<f32>(index) * 0.7f) - 5.0f,
                                    2.0f + (static_cast<f32>(index) * 0.2f), 0.0f});
        }
        for (u64 tick = 0; tick < 120; ++tick) {
            CY_REQUIRE(fixture.step(tick).has_value());
        }
        const Expected<StepStatistics, Error> statistics =
            fixture.server->statistics(fixture.world);
        CY_REQUIRE(statistics.has_value());
        CY_CHECK_EQ(statistics->body_count, 17U);
        CY_CHECK_EQ(statistics->tick, 119U);
    }

    jobs.shutdown();
}

namespace {

/// What the parallel raycast case hands every partition. Read-only apart from the counters, which
/// are atomic — a raycast that mutated simulation state would be a data race here rather than a
/// mystery three milestones later.
struct ParallelProbe {
    const PhysicsServer* server = nullptr;
    WorldHandle world;
    u64 expected_body = 0;
    std::atomic<u32> agreed{0};
    std::atomic<u32> disagreed{0};
};

void raycast_partition(const cy::jobs::TaskContext& context, u64 begin, u64 end,
                       void* user) noexcept {
    (void)context;
    auto* probe = static_cast<ParallelProbe*>(user);
    for (u64 index = begin; index < end; ++index) {
        RayCastInput ray;
        // Each entity casts from its own place, so the partitions are not all repeating one query
        // whose answer could have been cached on the first call.
        ray.origin = Vec3{(static_cast<f32>(index % 16U) * 0.25f) - 2.0f, 3.0f,
                          (static_cast<f32>(index % 7U) * 0.25f) - 0.75f};
        ray.direction = Vec3{0.0f, -1.0f, 0.0f};
        ray.max_distance = 10.0f;
        const Expected<RayCastHit, Error> hit =
            probe->server->raycast(probe->world, ray, QueryFilter{});
        const bool correct = hit.has_value() && hit->body.bits() == probe->expected_body;
        if (correct) {
            probe->agreed.fetch_add(1, std::memory_order_relaxed);
        } else {
            probe->disagreed.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

}  // namespace

CY_TEST_CASE("many entities raycast concurrently and every one gets the same answer") {
    // `physics` — "Parallel queries": "WHEN many entities raycast concurrently from a parallel
    // system THEN the queries SHALL be thread-safe and SHALL NOT mutate simulation state".
    //
    // Run over the ENGINE's job system rather than raw threads, because that is how a parallel
    // system will actually reach this code, and because it exercises the same scheduler the physics
    // step itself runs on. The state hash is taken before and after: a query that mutated the world
    // would change it, and "thread-safe" without "does not mutate" is only half the requirement.
    cy::jobs::JobSystem jobs;
    cy::jobs::JobSystemConfig config;
    config.worker_count = 4;
    CY_REQUIRE(jobs.start(config).has_value());

    {
        const Fixture fixture(&jobs);
        const BodyHandle floor = fixture.body(fixture.box(Vec3{10.0f, 0.5f, 10.0f}),
                                              MotionType::Static, Vec3{0.0f, -0.5f, 0.0f}, 1);
        CY_REQUIRE(fixture.step(0).has_value());

        determinism::StateHashTree before(allocator());
        CY_REQUIRE(fixture.server->hash_state(fixture.world, before).has_value());

        ParallelProbe probe;
        probe.server = fixture.server;
        probe.world = fixture.world;
        probe.expected_body = floor.bits();
        const Expected<cy::jobs::JobHandle, Error> submitted = jobs.submit_parallel_for(
            2048, 32, &raycast_partition, &probe, "physics.parallel-raycast");
        CY_REQUIRE(submitted.has_value());
        jobs.wait(*submitted);

        CY_CHECK_EQ(probe.agreed.load(std::memory_order_relaxed), 2048U);
        CY_CHECK_EQ(probe.disagreed.load(std::memory_order_relaxed), 0U);

        determinism::StateHashTree after(allocator());
        CY_REQUIRE(fixture.server->hash_state(fixture.world, after).has_value());
        CY_CHECK_EQ(before.root_hash(), after.root_hash());
        CY_CHECK_NE(after.root_hash(), 0U);
    }

    jobs.shutdown();
}

CY_TEST_CASE("a world torn down mid-step does not destroy the job pool underneath a worker") {
    // REGRESSION, M5.5's gate. `EngineJobSystem::QueueJob` hands a `JPH::Job` to an engine worker,
    // which calls `Execute()` and then `Release()` — and `Release()` reaches `FreeJob`, which
    // touches the fixed-size free list the destructor was about to destroy. The destructor was
    // `= default` and drained nothing, so a teardown that overtook an in-flight `physics.jolt` task
    // corrupted Jolt's free list. It reproduced about once in forty runs, as a heap fault inside
    // Jolt with no physics call on the stack.
    //
    // The shape that finds it is teardown UNDER LOAD: enough bodies that a step actually partitions
    // work, and a world destroyed immediately after submitting it rather than after a settled
    // simulation. M6 makes this far more likely than M5.5 did — streaming creates and destroys
    // worlds continuously rather than once per fixture.
    cy::jobs::JobSystem jobs;
    cy::jobs::JobSystemConfig config;
    config.worker_count = 4;
    CY_REQUIRE(jobs.start(config).has_value());

    for (u32 round = 0; round < 64; ++round) {
        const Fixture fixture(&jobs);
        CY_REQUIRE(fixture.server->capabilities().uses_engine_jobs);
        (void)fixture.body(fixture.box(Vec3{20.0f, 0.5f, 20.0f}), MotionType::Static,
                           Vec3{0.0f, -0.5f, 0.0f});
        const ShapeHandle sphere = fixture.sphere(0.35f);
        for (u32 index = 0; index < 48; ++index) {
            // A 12-wide grid, stacked: the integer division is the row and is meant to truncate.
            const u32 column = index % 12;
            const u32 row = index / 12;
            (void)fixture.body(sphere, MotionType::Dynamic,
                               Vec3{(static_cast<f32>(column) * 0.8f) - 4.0f,
                                    1.0f + (static_cast<f32>(index) * 0.15f),
                                    (static_cast<f32>(row) * 0.8f) - 1.0f});
        }
        // Two steps: the first populates the broad phase, the second is the one with real parallel
        // work in flight when the fixture goes out of scope on the next line.
        CY_REQUIRE(fixture.step(0).has_value());
        CY_REQUIRE(fixture.step(1).has_value());
    }

    // Reaching here without a fault is the assertion. The counter the fix added is private, so what
    // is checked is the observable consequence: the pool outlived every worker that touched it.
    CY_CHECK(jobs.is_running());
    jobs.shutdown();
}
