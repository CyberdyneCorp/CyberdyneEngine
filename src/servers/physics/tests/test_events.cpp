// Collision and trigger events: enter, stay, exit, filtering and the pair ordering. Task 4.2.4.

#include "fixture.h"

using namespace cy;
using namespace cy::physics;
using cy::physics::test::Fixture;

namespace {

/// The first event naming both bodies, or null. Every case here has at most one pair.
const ContactEvent* find_pair(Span<const ContactEvent> events, BodyHandle a,
                              BodyHandle b) noexcept {
    for (const ContactEvent& event : events) {
        if ((event.a == a && event.b == b) || (event.a == b && event.b == a)) {
            return &event;
        }
    }
    return nullptr;
}

}  // namespace

CY_TEST_CASE("a pair reports enter once, then stay, then exit once") {
    const Fixture fixture;
    const ShapeHandle shape = fixture.box(Vec3{0.5f, 0.5f, 0.5f});
    const BodyHandle wall = fixture.body(shape, MotionType::Static, Vec3{0, 0, 0}, 1);
    const BodyHandle mover = fixture.body(shape, MotionType::Kinematic, Vec3{3, 0, 0}, 2);
    CY_REQUIRE(
        fixture.server->set_body_velocity(mover, Vec3{-30.0f, 0.0f, 0.0f}, Vec3{}).has_value());

    bool saw_enter = false;
    bool saw_stay = false;
    bool saw_exit = false;
    u32 enters = 0;
    for (u64 tick = 0; tick < 30; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
        const Expected<Span<const ContactEvent>, Error> events =
            fixture.server->events(fixture.world);
        CY_REQUIRE(events.has_value());
        const ContactEvent* event = find_pair(*events, wall, mover);
        if (event == nullptr) {
            continue;
        }
        switch (event->phase) {
            case ContactPhase::Enter:
                ++enters;
                saw_enter = true;
                CY_CHECK_EQ(event->point_count, 1U);
                break;
            case ContactPhase::Stay:
                saw_stay = true;
                break;
            case ContactPhase::Exit:
                saw_exit = true;
                break;
        }
    }
    CY_CHECK(saw_enter);
    CY_CHECK(saw_stay);
    CY_CHECK(saw_exit);
    // Exactly one enter for one crossing. A backend that re-entered every tick would still see
    // "saw_enter", which is why the count is the assertion.
    CY_CHECK_EQ(enters, 1U);
}

CY_TEST_CASE("an event's pair is ordered and carries both bodies' user data") {
    // The order is the interface's, not the broad phase's: without it a pair produces one event in
    // one run and the mirrored event in another, depending on which body was visited first.
    const Fixture fixture;
    const ShapeHandle shape = fixture.box(Vec3{1.0f, 1.0f, 1.0f});
    const BodyHandle first = fixture.body(shape, MotionType::Static, Vec3{0, 0, 0}, 100);
    const BodyHandle second = fixture.body(shape, MotionType::Kinematic, Vec3{1, 0, 0}, 200);
    CY_REQUIRE(fixture.step(0).has_value());

    const Expected<Span<const ContactEvent>, Error> events = fixture.server->events(fixture.world);
    CY_REQUIRE(events.has_value());
    const ContactEvent* event = find_pair(*events, first, second);
    CY_REQUIRE(event != nullptr);
    CY_CHECK_LT(event->a.bits(), event->b.bits());
    CY_CHECK_EQ(event->other_than(event->a).bits(), event->b.bits());
    CY_CHECK_EQ(event->user_data_a + event->user_data_b, 300U);
}

CY_TEST_CASE("a trigger reports through the trigger channel and a solid pair does not") {
    const Fixture fixture;
    const ShapeHandle shape = fixture.box(Vec3{1.0f, 1.0f, 1.0f});
    const BodyHandle volume = fixture.body(shape, MotionType::Static, Vec3{0, 0, 0}, 1, true);
    const BodyHandle solid = fixture.body(shape, MotionType::Kinematic, Vec3{1, 0, 0}, 2);
    CY_REQUIRE(fixture.step(0).has_value());
    const ContactEvent* event = find_pair(*fixture.server->events(fixture.world), volume, solid);
    CY_REQUIRE(event != nullptr);
    CY_CHECK(event->trigger);
}

CY_TEST_CASE("a pair the filter rejects produces no event at all") {
    // `physics` — "One-way filtering is symmetric", end to end: A accepts B's layer, B does not
    // accept A's, and the pair does not collide. A single-sided implementation reports a contact
    // here.
    const Fixture fixture;
    const ShapeHandle shape = fixture.box(Vec3{1.0f, 1.0f, 1.0f});

    ColliderDescription a_collider;
    a_collider.shape = shape;
    a_collider.filter = CollisionFilter{1, 0xFFFFFFFFU};
    BodyDescription a;
    a.motion = MotionType::Static;
    a.colliders = &a_collider;
    a.collider_count = 1;
    const Expected<BodyHandle, Error> first = fixture.server->create_body(fixture.world, a);
    CY_REQUIRE(first.has_value());

    ColliderDescription b_collider;
    b_collider.shape = shape;
    b_collider.filter = CollisionFilter{2, 0};  // accepts nothing
    BodyDescription b;
    b.motion = MotionType::Kinematic;
    b.transform = Transform::from_translation(Vec3{1.0f, 0.0f, 0.0f});
    b.colliders = &b_collider;
    b.collider_count = 1;
    const Expected<BodyHandle, Error> second = fixture.server->create_body(fixture.world, b);
    CY_REQUIRE(second.has_value());

    CY_REQUIRE(fixture.step(0).has_value());
    CY_CHECK(find_pair(*fixture.server->events(fixture.world), *first, *second) == nullptr);
}

CY_TEST_CASE("an ignored pair produces no event, and un-ignoring restores it") {
    const Fixture fixture;
    const ShapeHandle shape = fixture.box(Vec3{1.0f, 1.0f, 1.0f});
    const BodyHandle first = fixture.body(shape, MotionType::Static, Vec3{0, 0, 0});
    const BodyHandle second = fixture.body(shape, MotionType::Kinematic, Vec3{1, 0, 0});

    CY_REQUIRE(fixture.server->set_pair_ignored(first, second, true).has_value());
    CY_REQUIRE(fixture.step(0).has_value());
    CY_CHECK(find_pair(*fixture.server->events(fixture.world), first, second) == nullptr);

    // Symmetric: the un-ignore is written with the arguments the other way round, because the rule
    // is mutual and an asymmetric ignore list would be the one place it is not.
    CY_REQUIRE(fixture.server->set_pair_ignored(second, first, false).has_value());
    CY_REQUIRE(fixture.step(1).has_value());
    CY_CHECK(find_pair(*fixture.server->events(fixture.world), first, second) != nullptr);
}

CY_TEST_CASE("a collider with report_stay off keeps enter and exit and drops the flood") {
    // `physics` — "Contact filtering": a resting stack otherwise costs one event per pair per tick
    // forever, which is the event flood the requirement is about.
    const Fixture fixture;
    const ShapeHandle shape = fixture.box(Vec3{1.0f, 1.0f, 1.0f});
    ColliderDescription quiet;
    quiet.shape = shape;
    quiet.report_stay = false;

    BodyDescription a;
    a.motion = MotionType::Static;
    a.colliders = &quiet;
    a.collider_count = 1;
    const Expected<BodyHandle, Error> first = fixture.server->create_body(fixture.world, a);
    CY_REQUIRE(first.has_value());

    BodyDescription b;
    b.motion = MotionType::Kinematic;
    b.transform = Transform::from_translation(Vec3{1.0f, 0.0f, 0.0f});
    b.colliders = &quiet;
    b.collider_count = 1;
    const Expected<BodyHandle, Error> second = fixture.server->create_body(fixture.world, b);
    CY_REQUIRE(second.has_value());

    CY_REQUIRE(fixture.step(0).has_value());
    const ContactEvent* entered =
        find_pair(*fixture.server->events(fixture.world), *first, *second);
    CY_REQUIRE(entered != nullptr);
    CY_CHECK_EQ(entered->phase, ContactPhase::Enter);

    for (u64 tick = 1; tick < 5; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
        CY_CHECK(find_pair(*fixture.server->events(fixture.world), *first, *second) == nullptr);
    }

    // And exit still arrives, which is what distinguishes "quiet" from "off".
    CY_REQUIRE(fixture.server
                   ->set_body_transform(*second, Transform::from_translation(Vec3{20, 0, 0}),
                                        TeleportMode::Teleport)
                   .has_value());
    CY_REQUIRE(fixture.step(5).has_value());
    const ContactEvent* exited = find_pair(*fixture.server->events(fixture.world), *first, *second);
    CY_REQUIRE(exited != nullptr);
    CY_CHECK_EQ(exited->phase, ContactPhase::Exit);
}

CY_TEST_CASE("the event buffer drops rather than allocating when its reservation is full") {
    EventBuffer buffer(cy::physics::test::allocator());
    CY_REQUIRE(buffer.reserve(2).has_value());
    ContactEvent event;
    event.a = BodyHandle::from_slot(1, 1);
    event.b = BodyHandle::from_slot(2, 1);
    CY_CHECK(buffer.push(event));
    CY_CHECK(buffer.push(event));
    // An allocation inside the step is what the reservation exists to prevent; a dropped event with
    // a count beside it is a diagnosable configuration problem instead.
    CY_CHECK_FALSE(buffer.push(event));
    CY_CHECK_EQ(buffer.size(), 2U);
    CY_CHECK_EQ(buffer.dropped(), 1U);
}

CY_TEST_CASE("pushing a reversed pair normalises the order and flips the normals") {
    EventBuffer buffer(cy::physics::test::allocator());
    CY_REQUIRE(buffer.reserve(1).has_value());
    ContactEvent event;
    event.a = BodyHandle::from_slot(9, 1);
    event.b = BodyHandle::from_slot(2, 1);
    event.user_data_a = 9;
    event.user_data_b = 2;
    event.point_count = 1;
    event.points[0].normal = Vec3{0.0f, 1.0f, 0.0f};
    CY_CHECK(buffer.push(event));
    const ContactEvent& stored = buffer.events()[0];
    CY_CHECK_LT(stored.a.bits(), stored.b.bits());
    CY_CHECK_EQ(stored.user_data_a, 2U);
    // The normal points from `a` to `b`, so swapping the pair must negate it — otherwise every
    // reader that pushed along it would push the wrong way for half the pairs.
    CY_CHECK_EQ(stored.points[0].normal.y, -1.0f);
}
