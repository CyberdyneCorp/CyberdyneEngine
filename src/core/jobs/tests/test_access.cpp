// Access declarations and the schedule derived from them. Task 3.2.2 — the milestone's invariant.
//
// Every case here is one of `core-jobs-and-concurrency`'s scenarios, exercised by synthetic systems
// because no real one exists until M2. That is the point of landing the model now: the scenarios
// can be satisfied before there is anything to schedule, and a system written at M2 is written
// against a checker that already refuses the shapes that cannot be scheduled.

#include "harness.h"

#include <cy/core/jobs/access.h>
#include <cy/core/jobs/schedule.h>

namespace {

using namespace cy;
using namespace cy::jobs;

// Synthetic component identifiers. Nothing derives meaning from the numbers — M2's ECS supplies
// real ones from the reflection manifest — which is exactly why a test can invent them.
constexpr ComponentTypeId kTransform = 1;
constexpr ComponentTypeId kVelocity = 2;
constexpr ComponentTypeId kInput = 3;
constexpr ComponentTypeId kHealth = 4;
constexpr ComponentTypeId kDamage = 5;
constexpr ComponentTypeId kFrozen = 6;

void empty_system(const SystemContext&, void*) noexcept {}

AccessSet reads(ComponentTypeId type) {
    AccessSet set;
    CY_CHECK(set.read(type).has_value());
    return set;
}

AccessSet writes(ComponentTypeId type) {
    AccessSet set;
    CY_CHECK(set.write(type).has_value());
    return set;
}

}  // namespace

CY_TEST_CASE("two reads of one component never conflict") {
    const AccessSet first = reads(kTransform);
    const AccessSet second = reads(kTransform);
    AccessConflict conflict;
    CY_CHECK_FALSE(first.conflicts_with(second, conflict));
}

CY_TEST_CASE("a write conflicts with any other access to the same component") {
    AccessConflict conflict;

    const AccessSet writer = writes(kTransform);
    CY_CHECK(writer.conflicts_with(reads(kTransform), conflict));
    CY_CHECK_EQ(conflict.id, kTransform);
    CY_CHECK_EQ(conflict.first, Access::Write);
    CY_CHECK_EQ(conflict.second, Access::Read);

    CY_CHECK(writer.conflicts_with(writes(kTransform), conflict));
    CY_CHECK_FALSE(writer.conflicts_with(writes(kVelocity), conflict));
}

CY_TEST_CASE("an exclude reads no data and therefore conflicts with nothing") {
    AccessSet excluding;
    CY_REQUIRE(excluding.exclude(kFrozen).has_value());

    AccessConflict conflict;
    CY_CHECK_FALSE(excluding.conflicts_with(writes(kFrozen), conflict));
    CY_CHECK_FALSE(excluding.conflicts_with(reads(kFrozen), conflict));
}

CY_TEST_CASE("the three identifier spaces are separate") {
    AccessSet component;
    CY_REQUIRE(component.write(7).has_value());
    AccessSet resource;
    CY_REQUIRE(resource.resource_write(7).has_value());
    AccessSet channel;
    CY_REQUIRE(channel.event_write(7).has_value());

    AccessConflict conflict;
    CY_CHECK_FALSE(component.conflicts_with(resource, conflict));
    CY_CHECK_FALSE(resource.conflicts_with(channel, conflict));
    CY_CHECK(component.conflicts_with(writes(7), conflict));
}

CY_TEST_CASE("a contradictory self-declaration is refused where it is written") {
    AccessSet set;
    CY_REQUIRE(set.read(kTransform).has_value());

    const auto contradiction = set.write(kTransform);
    CY_REQUIRE_FALSE(contradiction.has_value());
    CY_CHECK_EQ(contradiction.error().code, ErrorCode::InvalidArgument);

    const auto duplicate = set.read(kTransform);
    CY_REQUIRE_FALSE(duplicate.has_value());
    CY_CHECK_EQ(duplicate.error().code, ErrorCode::AlreadyExists);
}

// --- The schedule
// ---------------------------------------------------------------------------------

CY_TEST_CASE("independent systems share a batch") {
    // The specification's scenario: one system writes Velocity while reading Input, another writes
    // Health while reading Damage. Nothing they touch overlaps, so nothing orders them.
    SystemSchedule schedule;

    SystemDesc movement;
    movement.name = "movement";
    movement.body = &empty_system;
    CY_REQUIRE(movement.access.write(kVelocity).has_value());
    CY_REQUIRE(movement.access.read(kInput).has_value());

    SystemDesc combat;
    combat.name = "combat";
    combat.body = &empty_system;
    CY_REQUIRE(combat.access.write(kHealth).has_value());
    CY_REQUIRE(combat.access.read(kDamage).has_value());

    const auto first = schedule.add(movement);
    const auto second = schedule.add(combat);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_REQUIRE(schedule.build().has_value());

    CY_CHECK_EQ(schedule.batch_count(), 1u);
    CY_CHECK_EQ(schedule.batch_size(0), 2u);
    CY_CHECK_FALSE(schedule.ordered_before(first.value(), second.value()));
    CY_CHECK_FALSE(schedule.ordered_before(second.value(), first.value()));
}

CY_TEST_CASE("conflicting systems are serialised in a stable order") {
    SystemSchedule schedule;

    SystemDesc physics;
    physics.name = "physics";
    physics.body = &empty_system;
    CY_REQUIRE(physics.access.write(kTransform).has_value());

    SystemDesc animation;
    animation.name = "animation";
    animation.body = &empty_system;
    CY_REQUIRE(animation.access.write(kTransform).has_value());

    const auto first = schedule.add(physics);
    const auto second = schedule.add(animation);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());

    AccessConflict conflict;
    CY_CHECK(schedule.conflicts(first.value(), second.value(), conflict));
    CY_CHECK_EQ(conflict.id, kTransform);

    // The order is registration order, and it was decided by `add` rather than discovered by `run`.
    CY_CHECK(schedule.ordered_before(first.value(), second.value()));
    CY_CHECK_FALSE(schedule.ordered_before(second.value(), first.value()));

    CY_REQUIRE(schedule.build().has_value());
    CY_CHECK_EQ(schedule.batch_count(), 2u);
    CY_CHECK_EQ(schedule.batch_member(0, 0), first.value());
    CY_CHECK_EQ(schedule.batch_member(1, 0), second.value());
}

CY_TEST_CASE("an explicit constraint decides the order a conflict would otherwise") {
    SystemSchedule schedule;

    SystemDesc late;
    late.name = "late";
    late.body = &empty_system;
    CY_REQUIRE(late.access.write(kTransform).has_value());
    const auto late_id = schedule.add(late);
    CY_REQUIRE(late_id.has_value());

    SystemDesc early;
    early.name = "early";
    early.body = &empty_system;
    CY_REQUIRE(early.access.write(kTransform).has_value());
    const SystemId after[] = {late_id.value()};
    early.after = after;
    early.after_count = 1;

    const auto early_id = schedule.add(early);
    CY_REQUIRE(early_id.has_value());
    CY_CHECK(schedule.ordered_before(late_id.value(), early_id.value()));
}

CY_TEST_CASE("a schedule refuses what it cannot run") {
    SystemSchedule schedule;

    SystemDesc nameless;
    nameless.body = &empty_system;
    CY_CHECK_EQ(schedule.add(nameless).error().code, ErrorCode::InvalidArgument);

    SystemDesc bodyless;
    bodyless.name = "bodyless";
    CY_CHECK_EQ(schedule.add(bodyless).error().code, ErrorCode::InvalidArgument);

    SystemDesc one;
    one.name = "one";
    one.body = &empty_system;
    CY_REQUIRE(schedule.add(one).has_value());
    CY_CHECK_EQ(schedule.add(one).error().code, ErrorCode::AlreadyExists);

    SystemDesc dangling;
    dangling.name = "dangling";
    dangling.body = &empty_system;
    const SystemId missing[] = {99};
    dangling.after = missing;
    dangling.after_count = 1;
    CY_CHECK_EQ(schedule.add(dangling).error().code, ErrorCode::NotFound);
}

CY_TEST_CASE("an ordering cycle is refused, naming neither system as runnable") {
    SystemSchedule schedule;

    SystemDesc a;
    a.name = "a";
    a.body = &empty_system;
    SystemDesc b;
    b.name = "b";
    b.body = &empty_system;
    SystemDesc c;
    c.name = "c";
    c.body = &empty_system;

    const auto first = schedule.add(a);
    const auto second = schedule.add(b);
    const auto third = schedule.add(c);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_REQUIRE(third.has_value());

    CY_REQUIRE(schedule.order(first.value(), second.value()).has_value());
    CY_REQUIRE(schedule.order(second.value(), third.value()).has_value());

    // c before a would close the loop a -> b -> c -> a.
    const auto closing = schedule.order(third.value(), first.value());
    CY_REQUIRE_FALSE(closing.has_value());
    CY_CHECK_EQ(closing.error().code, ErrorCode::InvalidArgument);

    // The graph is still the one that was valid before the refusal.
    CY_REQUIRE(schedule.build().has_value());
    CY_CHECK_EQ(schedule.batch_count(), 3u);
}

CY_TEST_CASE("a system's undeclared access is caught and counted") {
    reset_undeclared_access_violations();

    AccessSet declared;
    CY_REQUIRE(declared.read(kTransform).has_value());
    CY_REQUIRE(declared.write(kVelocity).has_value());
    const SystemAccessGuard guard("movement", declared);

    // Declared exactly: allowed.
    CY_CHECK(guard.check(AccessDomain::Component, kTransform, Access::Read));
    CY_CHECK(guard.check(AccessDomain::Component, kVelocity, Access::Write));
    // A writer may read what it writes.
    CY_CHECK(guard.check(AccessDomain::Component, kVelocity, Access::Read));
    CY_CHECK_EQ(undeclared_access_violations(), 0u);

    // A reader may not write what it reads, and nothing may touch what was never declared.
    CY_CHECK_FALSE(guard.check(AccessDomain::Component, kTransform, Access::Write));
    CY_CHECK_FALSE(guard.check(AccessDomain::Component, kHealth, Access::Read));

    // The counter is compiled into every configuration, which is why this assertion holds in
    // Profile and Shipping as well as in Debug and Development.
    CY_CHECK_EQ(undeclared_access_violations(), 2u);
    CY_CHECK_EQ(last_undeclared_access_id(), kHealth);
    reset_undeclared_access_violations();
}

// --- Deferred structural changes
// -------------------------------------------------------------------

CY_TEST_CASE("structural changes commit in a deterministic order, not in completion order") {
    DeferredCommands store;
    CY_REQUIRE(store.initialize(64).has_value());

    // Two systems record interleaved, in the order two workers might have finished. The commit key
    // is (system, partition, sequence), so the order they were appended in is irrelevant.
    CommandRecorder late(&store, 7, 0);
    CommandRecorder early(&store, 3, 0);

    CY_REQUIRE(late.create_entity(100).has_value());
    CY_REQUIRE(early.create_entity(200).has_value());
    CY_REQUIRE(late.create_entity(101).has_value());
    CY_REQUIRE(early.destroy_entity(201).has_value());

    CY_CHECK_EQ(store.pending(), 4u);

    struct Applied {
        u64 entities[8] = {};
        u32 count = 0;
    };
    Applied applied;
    const u64 count = store.flush(
        [](const StructuralCommand& command, void* user) noexcept {
            auto* seen = static_cast<Applied*>(user);
            seen->entities[seen->count++] = command.entity;
        },
        &applied);

    CY_CHECK_EQ(count, 4u);
    CY_CHECK_EQ(applied.count, 4u);
    CY_CHECK_EQ(applied.entities[0], 200u);  // system 3, sequence 0
    CY_CHECK_EQ(applied.entities[1], 201u);  // system 3, sequence 1
    CY_CHECK_EQ(applied.entities[2], 100u);  // system 7, sequence 0
    CY_CHECK_EQ(applied.entities[3], 101u);  // system 7, sequence 1
    CY_CHECK_EQ(store.pending(), 0u);
}

CY_TEST_CASE("a full command store reports rather than growing") {
    DeferredCommands store;
    CY_REQUIRE(store.initialize(2).has_value());
    CommandRecorder recorder(&store, 0, 0);

    CY_CHECK(recorder.create_entity(1).has_value());
    CY_CHECK(recorder.create_entity(2).has_value());
    const auto refused = recorder.create_entity(3);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::OutOfRange);
    CY_CHECK_EQ(store.refused(), 1u);
}

CY_TEST_CASE("a recorder with no store reports instead of discarding") {
    CommandRecorder orphan(nullptr, 0, 0);
    const auto recorded = orphan.create_entity(1);
    CY_REQUIRE_FALSE(recorded.has_value());
    CY_CHECK_EQ(recorded.error().code, ErrorCode::Unavailable);
}
