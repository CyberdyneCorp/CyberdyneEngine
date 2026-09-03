// Running a stage built from access declarations. Task 3.2.2, the executable half.
//
// test_access.cpp fixes what the checker decides; this fixes what happens when the decision is
// executed on real workers: independent systems run concurrently, conflicting ones are serialised,
// structural changes recorded during parallel execution are applied at the flush point in commit
// order, and a system touching what it did not declare is caught.
//
// The systems are synthetic because no real one exists until M2. That is the point of landing the
// model now — the shape a system must be written in is fixed before anything is written in it.

#include "harness.h"

#include <cy/core/jobs/access.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/jobs/schedule.h>

#include <atomic>

namespace {

using namespace cy;
using namespace cy::jobs;
using cy::jobs::test::ScopedJobSystem;

constexpr ComponentTypeId kTransform = 1;
constexpr ComponentTypeId kVelocity = 2;
constexpr ComponentTypeId kInput = 3;
constexpr ComponentTypeId kHealth = 4;
constexpr ComponentTypeId kDamage = 5;

struct Stage {
    std::atomic<u32> concurrent{0};
    std::atomic<u32> peak_concurrent{0};
    std::atomic<u32> stamp{0};
    std::atomic<u32> physics_at{0};
    std::atomic<u32> animation_at{0};
    std::atomic<u32> ran{0};
};

void note_peak(Stage& stage) noexcept {
    const u32 now = stage.concurrent.fetch_add(1) + 1;
    u32 peak = stage.peak_concurrent.load();
    while (now > peak && !stage.peak_concurrent.compare_exchange_weak(peak, now)) {
    }
    // Long enough that two systems in one batch genuinely overlap, short enough for the budget.
    Thread::sleep_for_ns(2'000'000);
    stage.concurrent.fetch_sub(1);
    stage.ran.fetch_add(1);
}

void movement_system(const SystemContext& context, void* user) noexcept {
    auto* stage = static_cast<Stage*>(user);
    // The declaration check, at the point the body touches something. Live in every configuration
    // as a counter; asserted where assertions are.
    CY_ASSERT_DECLARED_ACCESS(context.guard, AccessDomain::Component, kVelocity, Access::Write);
    CY_ASSERT_DECLARED_ACCESS(context.guard, AccessDomain::Component, kInput, Access::Read);
    note_peak(*stage);
}

void combat_system(const SystemContext& context, void* user) noexcept {
    auto* stage = static_cast<Stage*>(user);
    CY_ASSERT_DECLARED_ACCESS(context.guard, AccessDomain::Component, kHealth, Access::Write);
    CY_ASSERT_DECLARED_ACCESS(context.guard, AccessDomain::Component, kDamage, Access::Read);
    note_peak(*stage);
}

void physics_system(const SystemContext&, void* user) noexcept {
    auto* stage = static_cast<Stage*>(user);
    Thread::sleep_for_ns(1'000'000);
    stage->physics_at.store(stage->stamp.fetch_add(1) + 1);
}

void animation_system(const SystemContext&, void* user) noexcept {
    auto* stage = static_cast<Stage*>(user);
    stage->animation_at.store(stage->stamp.fetch_add(1) + 1);
}

void spawning_system(const SystemContext& context, void* user) noexcept {
    auto* count = static_cast<std::atomic<u32>*>(user);
    // A system running in parallel may not create an entity; it records the intent, which is
    // applied at the stage's flush point in commit order.
    for (u64 entity = 0; entity < 4; ++entity) {
        if (context.commands != nullptr &&
            context.commands->create_entity(context.system * 100 + entity)) {
            count->fetch_add(1);
        }
    }
}

}  // namespace

CY_TEST_CASE("independent systems run concurrently on different workers") {
    ScopedJobSystem system(4);
    CY_REQUIRE(system.started());

    Stage stage;
    SystemSchedule schedule;

    SystemDesc movement;
    movement.name = "movement";
    movement.body = &movement_system;
    movement.user = &stage;
    CY_REQUIRE(movement.access.write(kVelocity).has_value());
    CY_REQUIRE(movement.access.read(kInput).has_value());

    SystemDesc combat;
    combat.name = "combat";
    combat.body = &combat_system;
    combat.user = &stage;
    CY_REQUIRE(combat.access.write(kHealth).has_value());
    CY_REQUIRE(combat.access.read(kDamage).has_value());

    CY_REQUIRE(schedule.add(movement).has_value());
    CY_REQUIRE(schedule.add(combat).has_value());
    reset_undeclared_access_violations();

    CY_REQUIRE(schedule.run(system.get()).has_value());

    CY_CHECK_EQ(stage.ran.load(), 2u);
    CY_CHECK_EQ(schedule.batch_count(), 1u);
    // The scheduler ran them on different workers in the same stage, which is the specification's
    // scenario stated as a measurement rather than as an intention.
    CY_CHECK_EQ(stage.peak_concurrent.load(), 2u);
    // Both bodies touched only what they declared.
    CY_CHECK_EQ(undeclared_access_violations(), 0u);
}

CY_TEST_CASE("two systems that write one component are serialised, in registration order") {
    ScopedJobSystem system(4);
    CY_REQUIRE(system.started());

    Stage stage;
    SystemSchedule schedule;

    SystemDesc physics;
    physics.name = "physics";
    physics.body = &physics_system;
    physics.user = &stage;
    CY_REQUIRE(physics.access.write(kTransform).has_value());

    SystemDesc animation;
    animation.name = "animation";
    animation.body = &animation_system;
    animation.user = &stage;
    CY_REQUIRE(animation.access.write(kTransform).has_value());

    CY_REQUIRE(schedule.add(physics).has_value());
    CY_REQUIRE(schedule.add(animation).has_value());
    CY_REQUIRE(schedule.run(system.get()).has_value());

    CY_CHECK_EQ(schedule.batch_count(), 2u);
    CY_CHECK_EQ(stage.physics_at.load(), 1u);
    CY_CHECK_EQ(stage.animation_at.load(), 2u);
}

CY_TEST_CASE("structural changes are deferred and applied at the flush point") {
    ScopedJobSystem system(4);
    CY_REQUIRE(system.started());

    DeferredCommands commands;
    CY_REQUIRE(commands.initialize(64).has_value());

    std::atomic<u32> recorded{0};
    SystemSchedule schedule;
    for (u32 i = 0; i < 3; ++i) {
        SystemDesc spawner;
        spawner.name = i == 0 ? "spawn.a" : (i == 1 ? "spawn.b" : "spawn.c");
        spawner.body = &spawning_system;
        spawner.user = &recorded;
        // Disjoint writes, so all three share a batch and record concurrently.
        CY_REQUIRE(spawner.access.write(10 + i).has_value());
        CY_REQUIRE(schedule.add(spawner).has_value());
    }

    struct Applied {
        u64 entities[16] = {};
        u32 count = 0;
    };
    Applied applied;

    CY_REQUIRE(schedule
                   .run(system.get(), &commands,
                        [](const StructuralCommand& command, void* user) noexcept {
                            auto* seen = static_cast<Applied*>(user);
                            if (seen->count < 16) {
                                seen->entities[seen->count++] = command.entity;
                            }
                        },
                        &applied)
                   .has_value());

    CY_CHECK_EQ(schedule.batch_count(), 1u);
    CY_CHECK_EQ(recorded.load(), 12u);
    CY_REQUIRE_EQ(applied.count, 12u);

    // Commit order is (system, partition, sequence): system 0's four spawns, then system 1's, then
    // system 2's — whichever worker happened to finish first.
    for (u32 i = 0; i < 12; ++i) {
        const u64 expected = static_cast<u64>(i / 4) * 100 + (i % 4);
        CY_REQUIRE_EQ(applied.entities[i], expected);
    }
    CY_CHECK_EQ(commands.pending(), 0u);
}

CY_TEST_CASE("a system touching what it did not declare is identified") {
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());
    reset_undeclared_access_violations();

    SystemSchedule schedule;
    SystemDesc offender;
    offender.name = "reads-what-it-did-not-declare";
    offender.body = [](const SystemContext& context, void*) noexcept {
        CY_ASSERT_DECLARED_ACCESS(context.guard, AccessDomain::Component, kHealth, Access::Read);
    };
    CY_REQUIRE(offender.access.read(kTransform).has_value());
    CY_REQUIRE(schedule.add(offender).has_value());

    // Only the counted half of the check can be exercised: the assertion aborts the process where
    // assertions are live, which is the correct behaviour and not something a test can survive.
    if (!cy::jobs::test::assertions_are_live()) {
        CY_REQUIRE(schedule.run(system.get()).has_value());
        CY_CHECK_EQ(undeclared_access_violations(), 1u);
        CY_CHECK_EQ(last_undeclared_access_id(), kHealth);
    }
    reset_undeclared_access_violations();
}

CY_TEST_CASE("a stage runs its batches in order and reports what it built") {
    ScopedJobSystem system(4);
    CY_REQUIRE(system.started());

    SystemSchedule schedule;
    std::atomic<u32> order{0};
    struct Step {
        std::atomic<u32>* order;
        std::atomic<u32> at{0};
    };
    static Step steps[4];

    for (u32 i = 0; i < 4; ++i) {
        steps[i].order = &order;
        steps[i].at.store(0);
        SystemDesc desc;
        static const char* names[] = {"s0", "s1", "s2", "s3"};
        desc.name = names[i];
        desc.user = &steps[i];
        desc.body = [](const SystemContext&, void* user) noexcept {
            auto* step = static_cast<Step*>(user);
            step->at.store(step->order->fetch_add(1) + 1);
        };
        // Every system writes the same component, so the whole stage is a chain.
        CY_REQUIRE(desc.access.write(kTransform).has_value());
        CY_REQUIRE(schedule.add(desc).has_value());
    }

    CY_REQUIRE(schedule.build().has_value());
    CY_CHECK_EQ(schedule.batch_count(), 4u);
    for (u32 i = 0; i < 4; ++i) {
        CY_CHECK_EQ(schedule.batch_size(i), 1u);
        CY_CHECK_EQ(schedule.batch_of(i), i);
    }

    CY_REQUIRE(schedule.run(system.get()).has_value());
    for (u32 i = 0; i < 4; ++i) {
        CY_REQUIRE_EQ(steps[i].at.load(), i + 1);
    }
}

CY_TEST_CASE("a stage run without a command store tells a system that records one") {
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());

    static std::atomic<u32> code{0};
    code.store(0);

    SystemSchedule schedule;
    SystemDesc desc;
    desc.name = "records-without-a-store";
    desc.body = [](const SystemContext& context, void*) noexcept {
        const Status recorded = context.commands->create_entity(1);
        code.store(recorded.has_value() ? 0u : static_cast<u32>(recorded.error().code));
    };
    CY_REQUIRE(schedule.add(desc).has_value());
    CY_REQUIRE(schedule.run(system.get()).has_value());

    CY_CHECK_EQ(code.load(), static_cast<u32>(ErrorCode::Unavailable));
}
