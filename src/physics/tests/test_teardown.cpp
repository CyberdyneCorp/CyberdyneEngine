// Tearing a physics world down under load. M8.a task 4.4.
//
// ================================================================================================
// WHY THIS FILE EXISTS, AND WHY IT IS NOT THE BACKEND'S TEST AGAIN
// ================================================================================================
//
// M5.5's gate found `EngineJobSystem`'s destructor freeing Jolt's fixed job pool underneath a
// worker still inside `Release()` — one run in forty, as a fault with no physics call on the stack.
// `src/backends/physics-jolt/tests/test_jolt.cpp` holds the regression for that defect, at the
// layer it was in: a world created, stepped twice and destroyed, sixty-four times.
//
// **This is the same shape one layer up, and the layer is the point.** M8.a's design.md §3: "Play
// mode creates and destroys worlds constantly, which is exactly the shape that found it." What play
// mode destroys is not a bare `JPH::PhysicsSystem` — it is an ECS world, a scene tree, a bridge
// holding a body per entity and a shape per collider, and a stepper holding an interpolation record
// per body. Each of those is a new opportunity to free something in the wrong order, and none of
// them existed when the backend's regression was written.
//
// The four orderings a play session can produce, all four exercised below:
//
//   1. the bridge torn down while the server and the world are alive     — the ordinary stop
//   2. the bridge torn down and then torn down again                     — a stop, then a shutdown
//   3. the PHYSICS WORLD destroyed before the bridge that holds its bodies
//                                                                        — a runtime dying under
//                                                                          an editor that is still
//                                                                          holding the session
//   4. everything destroyed and rebuilt, repeatedly, with jobs in flight — play, stop, play, stop
//
// ================================================================================================
// WHAT "UNDER LOAD" MEANS HERE, AND WHY IT IS NOT DECORATION
// ================================================================================================
//
// Two loads, and they find different things. The job system's own workers are what put a Jolt task
// in flight at the moment of teardown — that is M5.5's window, and without them the destructor
// races nothing. The spinner threads are what make the window WIDE: a machine with every core busy
// deschedules the worker holding the last job for milliseconds, which is the difference between a
// race that reproduces once in forty runs and one that reproduces most of the time.
//
// This suite is `integration`, not `unit`, for the same reason the backend's is: it steps real
// bodies enough times for the solver to partition work. The budget it is held to is the test
// thread's own CPU time, which the spinners do not spend.

#include "fixture.h"

#include <cy/core/jobs/job_system.h>
#include <cy/test/test.h>

#if defined(CY_PHYSICS)
#    include <cy/backends/physics/jolt/server.h>
#endif

#include <atomic>
#include <thread>
#include <vector>

using cy::f32;
using cy::u32;
using cy::u64;
using namespace cy::physics;
using namespace cy::physics::test;

namespace {

/// Threads that do nothing but burn a core, so that the thread holding the last job is descheduled
/// at the moment the teardown wants it to have finished.
class Load {
public:
    explicit Load(u32 threads) noexcept {
        for (u32 index = 0; index < threads; ++index) {
            workers_.emplace_back([this]() noexcept {
                u64 sink = 1;
                while (!stop_.load(std::memory_order_relaxed)) {
                    // Deliberately cheap: the point is to occupy a core, not to measure anything.
                    for (u32 spin = 0; spin < 4096; ++spin) {
                        sink = (sink * 6364136223846793005ULL) + 1442695040888963407ULL;
                    }
                    // PUBLISHED WHILE THE LOOP RUNS, not at the end. The first draft added this
                    // after the loop and the assertion below read zero, because the threads are
                    // still spinning when the case checks that they were: the check would have
                    // passed only if the load had already stopped, which is the opposite of what
                    // it is for.
                    burned_.fetch_add(sink | 1U, std::memory_order_relaxed);
                }
            });
        }
    }

    ~Load() {
        stop_.store(true, std::memory_order_relaxed);
        for (std::thread& worker : workers_) {
            worker.join();
        }
    }

    Load(const Load&) = delete;
    Load& operator=(const Load&) = delete;

    [[nodiscard]] u64 burned() const noexcept { return burned_.load(std::memory_order_relaxed); }

private:
    std::vector<std::thread> workers_;
    std::atomic<bool> stop_{false};
    std::atomic<u64> burned_{0};
};
}  // namespace

CY_TEST_CASE("tearing the bridge down releases every body and shape it created") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);

    for (u32 index = 0; index < 24; ++index) {
        const cy::ecs::Entity entity =
            fixture.node("Crate", cy::Vec3{static_cast<f32>(index), 4.0F, 0.0F});
        CY_REQUIRE(entity.valid());
        CY_REQUIRE(fixture.box(entity, cy::Vec3{0.5F, 0.5F, 0.5F}));
        CY_REQUIRE(fixture.dynamic(entity));
    }
    CY_REQUIRE(fixture.run(8).has_value());
    CY_CHECK_EQ(fixture.bridge->tracked_bodies(), 24U);
    CY_CHECK_EQ(fixture.bridge->statistics().shapes_created, 24U);

    // Twenty-four identical boxes are one shape in the server's cache and twenty-four references to
    // it, which is what the cache is for. Held here so the release can be observed after teardown.
    ShapeDescription box;
    box.type = ShapeType::Box;
    box.half_extents = cy::Vec3{0.5F, 0.5F, 0.5F};
    const auto shared = fixture.backend.server().create_shape(box);
    CY_REQUIRE(shared.has_value());
    CY_REQUIRE(fixture.backend.server().destroy_shape(*shared).has_value());

    fixture.bridge->teardown();
    CY_CHECK_EQ(fixture.bridge->tracked_bodies(), 0U);
    CY_CHECK_EQ(fixture.bridge->statistics().bodies_destroyed, 24U);
    CY_CHECK_EQ(fixture.bridge->stepper().tracked_count(), 0U);

    // THE SHAPES WENT BACK TOO, and this is how that is observable rather than asserted: the
    // server's shape cache is reference counted, so asking for the same box again returns the SAME
    // handle while anything still holds one and a handle with a bumped generation once the last
    // reference is gone. A bridge that released bodies and kept shapes would leak one reference per
    // collider per play session — the leak nobody notices until the fortieth run — and this
    // comparison would find the old handle instead.
    const auto again = fixture.backend.server().create_shape(box);
    CY_REQUIRE(again.has_value());
    CY_CHECK_NE(again->bits(), shared->bits());
    CY_REQUIRE(fixture.backend.server().destroy_shape(*again).has_value());

    // Idempotent — a stop followed by a shutdown must not double-free. This is ordering 2.
    fixture.bridge->teardown();
    CY_CHECK_EQ(fixture.bridge->statistics().bodies_destroyed, 24U);

    // And the torn-down bridge refuses work rather than reaching for a server it has let go of.
    CY_CHECK_FALSE(fixture.bridge->sync().has_value());
    CY_CHECK_FALSE(fixture.bridge->step(fixture.clock).has_value());
}

CY_TEST_CASE("the physics world dying before the bridge is survivable, not a fault") {
    // ORDERING 3, and it is not hypothetical: `editor-rust-application` requires that "a runtime
    // failure SHALL NOT terminate the editor". The runtime's world can therefore be gone while the
    // structures above it are still being unwound, and the bridge must not turn that into a second
    // failure on top of the first.
    Fixture fixture;
    CY_REQUIRE(fixture.started);

    for (u32 index = 0; index < 8; ++index) {
        const cy::ecs::Entity entity =
            fixture.node("Crate", cy::Vec3{static_cast<f32>(index), 4.0F, 0.0F});
        CY_REQUIRE(entity.valid());
        CY_REQUIRE(fixture.box(entity, cy::Vec3{0.5F, 0.5F, 0.5F}));
        CY_REQUIRE(fixture.dynamic(entity));
    }
    CY_REQUIRE(fixture.run(4).has_value());
    CY_CHECK_EQ(fixture.bridge->tracked_bodies(), 8U);

    CY_REQUIRE(fixture.backend.server().destroy_world(fixture.backend.world()).has_value());

    // Every `destroy_body` below names a body in a world that no longer exists. The backend answers
    // with a diagnostic, the bridge drops it, and the process survives — which is the whole
    // assertion. `teardown()` swallowing the status is deliberate and is why it returns void.
    fixture.bridge->teardown();
    CY_CHECK_EQ(fixture.bridge->tracked_bodies(), 0U);
}

CY_TEST_CASE("sixty-four play sessions under load, each torn down with work in flight") {
    // ORDERING 4: the play/stop loop, which is what M8.a adds to the shape M5.5 found. Sixty-four
    // rounds because the defect it is modelled on reproduced about once in forty.
    cy::jobs::JobSystem jobs;
    cy::jobs::JobSystemConfig config;
    config.worker_count = 4;
    CY_REQUIRE(jobs.start(config).has_value());

    const Load load(4);

    for (u32 round = 0; round < 64; ++round) {
        Fixture fixture;
        CY_REQUIRE(fixture.started);

        const cy::ecs::Entity ground = fixture.node("Ground", cy::Vec3{0.0F, -0.5F, 0.0F});
        CY_REQUIRE(ground.valid());
        CY_REQUIRE(fixture.box(ground, cy::Vec3{20.0F, 0.5F, 20.0F}));
        CY_REQUIRE(fixture.immovable(ground));

        for (u32 index = 0; index < 32; ++index) {
            const u32 column = index % 8;
            const u32 row = index / 8;
            const cy::ecs::Entity entity =
                fixture.node("Crate", cy::Vec3{(static_cast<f32>(column) * 0.8F) - 3.0F,
                                               1.0F + (static_cast<f32>(index) * 0.15F),
                                               (static_cast<f32>(row) * 0.8F) - 1.0F});
            CY_REQUIRE(entity.valid());
            CY_REQUIRE(fixture.box(entity, cy::Vec3{0.35F, 0.35F, 0.35F}));
            CY_REQUIRE(fixture.dynamic(entity));
        }

        // Two steps: the first populates the broad phase, the second is the one with work in flight
        // when the fixture goes out of scope at the end of this iteration. The bridge is destroyed
        // FIRST — it is declared after the backend inside `Fixture`, so it is destroyed before it —
        // and then the world, then the server. Getting that order wrong is the defect.
        CY_REQUIRE(fixture.run(2).has_value());
        CY_CHECK_EQ(fixture.bridge->tracked_bodies(), 33U);
    }

    // Reaching here without a fault is the assertion, exactly as it is in the backend's own
    // regression. What is checked besides is that nothing took the scheduler down with it.
    CY_CHECK(jobs.is_running());
    CY_CHECK_NE(load.burned(), 0U);
    jobs.shutdown();
}

#if defined(CY_PHYSICS)

CY_TEST_CASE("the same play/stop loop against Jolt, with its job bridge in flight") {
    // The reference backend runs everything on the calling thread, so the loop above cannot race a
    // worker however loaded the machine is. THIS is the one that can: Jolt partitions a step across
    // the engine's job system, and a teardown that overtook a task in flight is the M5.5 defect
    // exactly. The backend's own regression covers a bare world; this covers a world with a bridge,
    // a scene tree and thirty-three entities on top of it.
    cy::jobs::JobSystem jobs;
    cy::jobs::JobSystemConfig config;
    config.worker_count = 4;
    CY_REQUIRE(jobs.start(config).has_value());

    const Load load(4);

    for (u32 round = 0; round < 64; ++round) {
        const cy::Expected<PhysicsServer*, cy::Error> made =
            jolt::create_server(allocator(), &jobs);
        CY_REQUIRE(made.has_value());
        PhysicsServer* server = *made;
        CY_REQUIRE(server->initialize().has_value());
        CY_CHECK(server->capabilities().uses_engine_jobs);

        WorldDescription description;
        description.name = cy::Name::intern("play");
        description.body_capacity = 256;
        description.body_pair_capacity = 2048;
        description.contact_constraint_capacity = 2048;
        const cy::Expected<WorldHandle, cy::Error> created = server->create_world(description);
        CY_REQUIRE(created.has_value());

        {
            cy::ecs::World world(allocator());
            CY_REQUIRE(world.initialize().has_value());
            cy::scene::SceneTree tree(world);
            CY_REQUIRE(tree.initialize().has_value());
            const auto components = PhysicsComponents::register_all(world);
            CY_REQUIRE(components.has_value());

            PhysicsBridge bridge(allocator(), tree, *components, *server, *created);
            cy::determinism::SimulationClock clock;
            cy::determinism::ClockConfig clock_config;
            clock_config.mode = cy::determinism::TickMode::FixedStep;
            CY_REQUIRE(clock.configure(clock_config).has_value());

            const auto place = [&](const char* name, cy::Vec3 at) noexcept {
                auto node = tree.create_node(cy::Name::intern(name), tree.root(), cy::Name());
                CY_REQUIRE(node.has_value());
                cy::scene::LocalTransform local;
                local.value = cy::Transform::from_translation(at);
                CY_REQUIRE(world.set(node->entity(), tree.components().local_transform, local)
                               .has_value());
                return node->entity();
            };

            const cy::ecs::Entity ground = place("Ground", cy::Vec3{0.0F, -0.5F, 0.0F});
            Collider slab;
            slab.shape.type = ShapeType::Box;
            slab.shape.half_extents = cy::Vec3{20.0F, 0.5F, 20.0F};
            CY_REQUIRE(world.add(ground, components->collider, &slab).has_value());
            StaticBody immovable;
            CY_REQUIRE(world.add(ground, components->static_body, &immovable).has_value());

            for (u32 index = 0; index < 48; ++index) {
                const u32 column = index % 12;
                const u32 row = index / 12;
                const cy::ecs::Entity entity =
                    place("Ball", cy::Vec3{(static_cast<f32>(column) * 0.8F) - 4.0F,
                                           1.0F + (static_cast<f32>(index) * 0.15F),
                                           (static_cast<f32>(row) * 0.8F) - 1.0F});
                Collider ball;
                ball.shape.type = ShapeType::Sphere;
                ball.shape.radius = 0.35F;
                CY_REQUIRE(world.add(entity, components->collider, &ball).has_value());
                RigidBody dynamic;
                dynamic.allow_sleeping = false;
                CY_REQUIRE(world.add(entity, components->rigid_body, &dynamic).has_value());
            }

            clock.advance();
            CY_REQUIRE(bridge.advance(clock).has_value());
            clock.advance();
            CY_REQUIRE(bridge.advance(clock).has_value());
            CY_CHECK_EQ(bridge.tracked_bodies(), 49U);
            // The bridge, the tree and the world die here, with the second step's tasks possibly
            // still draining. Nothing is waited for deliberately: waiting would be testing a
            // different program.
        }

        CY_REQUIRE(server->destroy_world(*created).has_value());
        server->shutdown();
        jolt::destroy_server(server, allocator());
    }

    CY_CHECK(jobs.is_running());
    CY_CHECK_NE(load.burned(), 0U);
    jobs.shutdown();
}

#endif  // CY_PHYSICS
