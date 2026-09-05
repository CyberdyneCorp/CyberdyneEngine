// The capsule character controller: ground, slopes, stairs, ceilings, platforms and the floating
// mode. Task 4.2.5.
//
// EVERY CASE HERE IS A CONFORMANCE CASE. The controller is engine code over `PhysicsServer`'s
// queries (character.h says why), so the same file compiled against a different backend is a test
// of that backend's queries. What is asserted is what the requirement names — grounded or not,
// stepped or blocked — never a millimetre, because the reference backend collides bounding volumes
// and Jolt collides shapes and the two legitimately differ in the last digit.

#include "fixture.h"

#include <cy/servers/physics/character.h>

using namespace cy;
using namespace cy::physics;
using cy::physics::test::Fixture;

namespace {

constexpr f32 kStep = 1.0f / 60.0f;

/// A floor, and a controller standing on it. The capsule is 1.8 m tall with a 0.3 m radius, so its
/// centre rests 0.9 m above the surface.
struct Scene {
    explicit Scene(const Fixture& fixture_, f32 floor_top = 0.0f) noexcept : fixture(&fixture_) {
        // A thick slab rather than a plane: the stair and platform cases need a finite surface, and
        // using the same floor everywhere keeps one thing constant across the cases.
        floor = fixture->body(fixture->box(Vec3{20.0f, 0.5f, 20.0f}), MotionType::Static,
                              Vec3{0.0f, floor_top - 0.5f, 0.0f});
    }

    [[nodiscard]] static CharacterDescription description(Vec3 start) noexcept {
        CharacterDescription description;
        description.start = Transform::from_translation(start);
        return description;
    }

    const Fixture* fixture;
    BodyHandle floor;
};

/// Settle the controller for `steps` fixed steps with a constant desired velocity.
void walk(CharacterController& character, Vec3 velocity, u32 steps) noexcept {
    CharacterInput input;
    input.desired_velocity = velocity;
    for (u32 index = 0; index < steps; ++index) {
        CY_REQUIRE(character.move(kStep, input).has_value());
    }
}

}  // namespace

CY_TEST_CASE("a capsule whose height does not exceed its diameter is rejected") {
    CharacterDescription description;
    description.radius = 0.5f;
    description.height = 1.0f;  // exactly a sphere
    CY_CHECK_FALSE(validate(description).has_value());
    description.height = 1.8f;
    CY_CHECK(validate(description).has_value());
    // A skin width at or past the radius would make every sweep stop before it started.
    description.skin_width = 0.5f;
    CY_CHECK_FALSE(validate(description).has_value());
}

CY_TEST_CASE("a character falls, lands, and is reported grounded") {
    const Fixture fixture;
    const Scene scene(fixture);
    CharacterController character(*fixture.server, fixture.world);
    CY_REQUIRE(character.create(scene.description(Vec3{0.0f, 3.0f, 0.0f})).has_value());
    CY_CHECK_EQ(character.state().ground, GroundState::InAir);

    walk(character, Vec3{}, 90);
    CY_CHECK_EQ(character.state().ground, GroundState::Grounded);
    // The capsule's centre sits a half-height above the floor, within the skin width.
    CY_CHECK_NEAR(character.state().transform.translation.y, 0.9f, 0.1f);
    // Landing zeroes the fall. Without it a character standing still accumulates an arbitrarily
    // large downward velocity and is launched off the first ramp it crosses.
    CY_CHECK_NEAR(character.state().velocity.y, 0.0f, 1e-3f);
}

CY_TEST_CASE("a character is lifted onto a step below the step offset") {
    // `physics` — "Walking up stairs": "WHEN a character moves into a step below the step offset
    // THEN it SHALL be lifted onto the step without a jump".
    const Fixture fixture;
    const Scene scene(fixture);
    // A 0.25 m step, inside the 0.35 m default offset.
    (void)fixture.body(fixture.box(Vec3{2.0f, 0.125f, 2.0f}), MotionType::Static,
                       Vec3{2.0f, 0.125f, 0.0f});

    CharacterController character(*fixture.server, fixture.world);
    CY_REQUIRE(character.create(scene.description(Vec3{-1.0f, 0.9f, 0.0f})).has_value());
    walk(character, Vec3{}, 10);
    CY_REQUIRE_EQ(character.state().ground, GroundState::Grounded);

    walk(character, Vec3{2.0f, 0.0f, 0.0f}, 90);
    CY_CHECK_GT(character.state().transform.translation.x, 1.0f);
    // On the step, not in front of it: the floor is at 0.9 and the step's top puts the centre at
    // 1.15. Half a step offset is the boundary that distinguishes the two.
    CY_CHECK_GT(character.state().transform.translation.y, 0.9f + 0.15f);
    CY_CHECK_EQ(character.state().ground, GroundState::Grounded);
}

CY_TEST_CASE("a wall taller than the step offset blocks rather than lifting") {
    // The control for the case above. Without it, a controller that teleported upward whenever it
    // was blocked would pass "walking up stairs" and climb walls.
    const Fixture fixture;
    const Scene scene(fixture);
    (void)fixture.body(fixture.box(Vec3{0.5f, 1.5f, 2.0f}), MotionType::Static,
                       Vec3{2.0f, 1.5f, 0.0f});

    CharacterController character(*fixture.server, fixture.world);
    CY_REQUIRE(character.create(scene.description(Vec3{0.0f, 0.9f, 0.0f})).has_value());
    walk(character, Vec3{}, 10);
    walk(character, Vec3{2.0f, 0.0f, 0.0f}, 90);

    CY_CHECK(character.state().touching_wall);
    CY_CHECK_LT(character.state().transform.translation.x, 1.55f);
    CY_CHECK_NEAR(character.state().transform.translation.y, 0.9f, 0.1f);
}

CY_TEST_CASE("a slope past the maximum reports a wall rather than ground") {
    // `physics` — "Steep slope": "WHEN a slope exceeds the maximum angle THEN the character SHALL
    // be reported as touching a wall rather than grounded, and SHALL slide down".
    const Fixture fixture;
    // A 60-degree half-space, past the 45-degree default. A plane, because it is the one shape the
    // reference backend has an exact normal for — an axis-aligned box could not express a slope.
    const f32 angle = 60.0f * cy::math::kDegToRad;
    const Vec3 normal = normalize(Vec3{-std::sin(angle), std::cos(angle), 0.0f});
    (void)fixture.body(fixture.ground_plane(normal), MotionType::Static, Vec3{});

    CharacterController character(*fixture.server, fixture.world);
    CharacterDescription description;
    description.start = Transform::from_translation(Vec3{0.0f, 3.0f, 0.0f});
    CY_REQUIRE(character.create(description).has_value());

    walk(character, Vec3{}, 90);
    CY_CHECK_NE(character.state().ground, GroundState::Grounded);
    CY_CHECK(character.state().touching_wall);
    // AND IT SLID, DOWNHILL. The slope's normal leans towards -X, so gravity projected onto it
    // pushes the character that way. A controller that reported the steep slope correctly and then
    // stood on it would pass the two assertions above and fail this one, which is the half of
    // `physics`' "SHALL slide down" that is easy to leave out.
    CY_CHECK_LT(character.state().transform.translation.x, -0.05f);
}

CY_TEST_CASE("a jump leaves the ground and gravity brings it back") {
    const Fixture fixture;
    const Scene scene(fixture);
    CharacterController character(*fixture.server, fixture.world);
    CY_REQUIRE(character.create(scene.description(Vec3{0.0f, 0.9f, 0.0f})).has_value());
    walk(character, Vec3{}, 10);
    CY_REQUIRE_EQ(character.state().ground, GroundState::Grounded);

    CharacterInput jump;
    jump.jump = true;
    jump.jump_speed = 5.0f;
    CY_REQUIRE(character.move(kStep, jump).has_value());
    CY_CHECK_GT(character.state().velocity.y, 0.0f);
    walk(character, Vec3{}, 10);
    CY_CHECK_EQ(character.state().ground, GroundState::InAir);
    CY_CHECK_GT(character.state().transform.translation.y, 1.2f);

    walk(character, Vec3{}, 120);
    CY_CHECK_EQ(character.state().ground, GroundState::Grounded);
}

CY_TEST_CASE("a ceiling stops a jump instead of letting it pass through") {
    const Fixture fixture;
    const Scene scene(fixture);
    (void)fixture.body(fixture.box(Vec3{5.0f, 0.25f, 5.0f}), MotionType::Static,
                       Vec3{0.0f, 2.5f, 0.0f});
    CharacterController character(*fixture.server, fixture.world);
    CY_REQUIRE(character.create(scene.description(Vec3{0.0f, 0.9f, 0.0f})).has_value());
    walk(character, Vec3{}, 10);

    CharacterInput jump;
    jump.jump = true;
    jump.jump_speed = 12.0f;
    CY_REQUIRE(character.move(kStep, jump).has_value());

    // OBSERVED AS IT HAPPENS, not read at the end. `touching_ceiling` describes THIS step, and it
    // is cleared at the top of every `move()` — a case that jumped, stepped twenty more times and
    // then read the flag would be reading the step on which the character was already falling
    // again, and would fail for a reason that has nothing to do with ceilings.
    bool struck = false;
    f32 highest = character.state().transform.translation.y;
    for (u32 index = 0; index < 40; ++index) {
        CY_REQUIRE(character.move(kStep, CharacterInput{}).has_value());
        struck = struck || character.state().touching_ceiling;
        highest = math::max(highest, character.state().transform.translation.y);
    }
    CY_CHECK(struck);
    // And it stopped UNDER the ceiling, whose underside is at 2.25 and whose capsule top is 0.9
    // above the centre — so a character that passed through would be well above 1.4.
    CY_CHECK_LT(highest, 1.4f);
    // The control: the jump was real. Without it a controller that never moved at all would report
    // no ceiling strike and a maximum height under 1.4, and pass the two checks above.
    CY_CHECK_GT(highest, 1.2f);
}

CY_TEST_CASE("a character on a moving platform is carried by it") {
    // `physics` — "Moving platform": "WHEN a character stands on a kinematic platform that moves
    // THEN the platform's motion SHALL be applied to the character".
    const Fixture fixture;
    const BodyHandle platform =
        fixture.body(fixture.box(Vec3{3.0f, 0.5f, 3.0f}), MotionType::Kinematic, Vec3{0, -0.5f, 0});
    CY_REQUIRE(
        fixture.server->set_body_velocity(platform, Vec3{1.0f, 0.0f, 0.0f}, Vec3{}).has_value());

    CharacterController character(*fixture.server, fixture.world);
    CharacterDescription description;
    description.start = Transform::from_translation(Vec3{0.0f, 0.9f, 0.0f});
    CY_REQUIRE(character.create(description).has_value());

    for (u32 index = 0; index < 60; ++index) {
        CY_REQUIRE(fixture.step(index).has_value());
        CY_REQUIRE(character.move(kStep, CharacterInput{}).has_value());
    }
    CY_CHECK_EQ(character.state().ground, GroundState::Grounded);
    CY_CHECK_GT(character.state().platform_velocity.x, 0.5f);
    // Carried roughly a metre in a second. The tolerance is wide because the character is following
    // a body it also sweeps against; what the requirement asks is that it moved WITH the platform
    // rather than being left behind.
    CY_CHECK_GT(character.state().transform.translation.x, 0.5f);
}

CY_TEST_CASE("a floating character ignores gravity and moves in six degrees of freedom") {
    const Fixture fixture;
    const Scene scene(fixture);
    CharacterController character(*fixture.server, fixture.world);
    CharacterDescription description = scene.description(Vec3{0.0f, 5.0f, 0.0f});
    description.mode = CharacterMode::Floating;
    CY_REQUIRE(character.create(description).has_value());

    walk(character, Vec3{}, 60);
    // No gravity: a floating character that drifted downward would be a grounded one with the mode
    // ignored.
    CY_CHECK_NEAR(character.state().transform.translation.y, 5.0f, 1e-3f);
    CY_CHECK_EQ(character.state().ground, GroundState::InAir);

    walk(character, Vec3{0.0f, 1.0f, 0.0f}, 60);
    CY_CHECK_GT(character.state().transform.translation.y, 5.5f);
}

CY_TEST_CASE("a character's own body is never a hit in its own sweeps") {
    const Fixture fixture;
    const Scene scene(fixture);
    CharacterController character(*fixture.server, fixture.world);
    CY_REQUIRE(character.create(scene.description(Vec3{0.0f, 0.9f, 0.0f}), true).has_value());
    CY_CHECK_FALSE(character.body().is_null());
    walk(character, Vec3{1.0f, 0.0f, 0.0f}, 30);
    // Without the self-exclusion the first sweep reports the character's own kinematic body at
    // distance zero and it never moves at all.
    CY_CHECK_GT(character.state().transform.translation.x, 0.1f);
    CY_CHECK_EQ(character.state().ground, GroundState::Grounded);
}

CY_TEST_CASE("a teleport moves the character and clears its velocity") {
    const Fixture fixture;
    const Scene scene(fixture);
    CharacterController character(*fixture.server, fixture.world);
    CY_REQUIRE(character.create(scene.description(Vec3{0.0f, 3.0f, 0.0f})).has_value());
    walk(character, Vec3{}, 30);
    CY_CHECK_LT(character.state().velocity.y, 0.0f);

    CY_REQUIRE(
        character.teleport(Transform::from_translation(Vec3{50.0f, 20.0f, 0.0f})).has_value());
    CY_CHECK_EQ(character.state().transform.translation.x, 50.0f);
    CY_CHECK_EQ(character.state().velocity.y, 0.0f);
    CY_CHECK_EQ(character.state().ground, GroundState::InAir);
}
