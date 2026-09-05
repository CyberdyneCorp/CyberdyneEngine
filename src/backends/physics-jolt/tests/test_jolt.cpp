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
    CY_CHECK_EQ(capabilities.determinism, DeterminismPolicy::SamePlatformDeterministic);
    // With no engine job system given, the work runs on the calling thread and the flag says so
    // rather than claiming a bridge that is not there.
    CY_CHECK_FALSE(capabilities.uses_engine_jobs);
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
