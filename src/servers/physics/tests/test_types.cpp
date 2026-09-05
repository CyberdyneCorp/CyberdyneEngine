// The physics vocabulary: collision filtering, the project matrix, material combining and the two
// validations. Task 4.2.1 and 4.2.4.

#include <cy/servers/physics/types.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::physics;

CY_TEST_CASE("a filter pair collides only when BOTH masks accept the other's layer") {
    // `physics` — "One-way filtering is symmetric": "WHEN A's mask includes B's layer but not vice
    // versa THEN the pair SHALL NOT collide, because the filter requires mutual acceptance". This
    // is the requirement most likely to be implemented as a single-sided test, which works in every
    // case where both sides happen to be symmetric — that is, in every manual test.
    const CollisionFilter a{1, 1U << 2U};
    const CollisionFilter b{2, 0};

    CY_CHECK_FALSE(accepts(a, b));
    CY_CHECK_FALSE(accepts(b, a));

    const CollisionFilter mutual{2, 1U << 1U};
    CY_CHECK(accepts(a, mutual));
    CY_CHECK(accepts(mutual, a));
}

CY_TEST_CASE("the collision matrix is symmetric by construction") {
    CollisionMatrix matrix;
    CY_CHECK(matrix.allows(3, 7));
    matrix.allow(3, 7, false);
    // Both halves. A matrix that can be asymmetric is a matrix whose two halves disagree in exactly
    // the case nobody tests.
    CY_CHECK_FALSE(matrix.allows(3, 7));
    CY_CHECK_FALSE(matrix.allows(7, 3));
    matrix.allow(7, 3, true);
    CY_CHECK(matrix.allows(3, 7));
    CY_CHECK(matrix.allows(7, 3));
}

CY_TEST_CASE("the matrix and the masks must both accept") {
    CollisionMatrix matrix;
    const CollisionFilter a{1, 0xFFFFFFFFU};
    const CollisionFilter b{2, 0xFFFFFFFFU};
    CY_CHECK(pair_collides(matrix, a, b));
    matrix.allow(1, 2, false);
    CY_CHECK_FALSE(pair_collides(matrix, a, b));
}

CY_TEST_CASE("combining friction takes the more restrictive of the two modes") {
    CY_CHECK_EQ(combine(0.4f, CombineMode::Average, 0.6f, CombineMode::Average), 0.5f);
    // Minimum outranks Average whichever side it is on, so a low-friction surface stays
    // low-friction however it is struck.
    CY_CHECK_EQ(combine(0.4f, CombineMode::Minimum, 0.6f, CombineMode::Average), 0.4f);
    CY_CHECK_EQ(combine(0.6f, CombineMode::Average, 0.4f, CombineMode::Minimum), 0.4f);
    CY_CHECK_EQ(combine(0.5f, CombineMode::Maximum, 0.2f, CombineMode::Average), 0.5f);
    CY_CHECK_EQ(combine(0.5f, CombineMode::Multiply, 0.4f, CombineMode::Multiply), 0.2f);
}

CY_TEST_CASE("a tuning whose slop exceeds its speculative distance is rejected") {
    Tuning tuning;
    CY_CHECK(validate(tuning).has_value());

    // The two numbers are independently plausible and wrong together: a contact is only created
    // after the overlap it was meant to prevent. Neither one's own range would catch it.
    tuning.penetration_slop = 0.1f;
    tuning.speculative_contact_distance = 0.02f;
    CY_CHECK_FALSE(validate(tuning).has_value());

    tuning = Tuning{};
    tuning.velocity_iterations = 0;
    CY_CHECK_FALSE(validate(tuning).has_value());

    tuning = Tuning{};
    tuning.baumgarte = 1.5f;
    CY_CHECK_FALSE(validate(tuning).has_value());
}

CY_TEST_CASE("a world with a zero capacity is rejected") {
    WorldDescription description;
    CY_CHECK(validate(description).has_value());
    description.body_capacity = 0;
    CY_CHECK_FALSE(validate(description).has_value());
}
