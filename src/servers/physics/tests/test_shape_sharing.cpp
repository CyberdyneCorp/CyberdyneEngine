// Shape sharing at the scale the requirement names — an INTEGRATION case, because a thousand shape
// creations do not fit a one-millisecond unit budget.
//
// WHY IT IS HERE RATHER THAN IN test_server.cpp. It was a `unit.physics_server` case until M6's
// closing gate, where `m1:four-profiles` — a criterion in the permanent set since M1, run by every
// later ledger — went red in the **Debug** configuration on this one case:
//
//     src/servers/physics/tests/test_server.cpp:23: ERROR: over budget: 'one thousand identical box
//     colliders create one shape' spent 1.573 ms of CPU against a budget of 1.000 ms
//
// Measured at that gate on an idle machine: **22 of 40** runs of the case alone exceeded the
// budget, between 1.06 ms and 2.10 ms of its own CPU time, and about one `ctest` run in ten. It is
// not a flake in the machine and it is not a regression — `src/servers/physics/` has not changed
// since M4 — it is a case that costs more than the taxonomy's unit budget in a configuration built
// at `-O0`, and it has been quietly failing one Debug run in ten ever since.
//
// The remedy is the one the budget's own diagnostic names and the one `testing-and-quality` states:
// "The taxonomy in `testing-and-quality` places a test this expensive in the next suite up — move
// it, or make it cheaper." Moving it is right rather than trimming it, because the requirement is
// about **one thousand** entities: cutting the count to fit a unit budget would be narrowing the
// claim to fit the measurement. `src/save/`, `tools/import/` and `src/servers/render/` each moved a
// case for the same reason during M6.
//
// WHAT IS ASSERTED IS UNCHANGED, LINE FOR LINE, from what `test_server.cpp` asserted.

#include "fixture.h"

#include <cy/servers/physics/reference/server.h>

using namespace cy;
using namespace cy::physics;
using cy::physics::test::Fixture;

CY_TEST_CASE("one thousand identical box colliders create one shape") {
    // `physics` — "Shape sharing": "WHEN 1 000 entities use an identical box collider THEN one Jolt
    // shape SHALL be created and referenced by all of them". Asserted through the statistics rather
    // than inferred from a handle comparison, because two calls returning the same handle would
    // also be true of a backend that leaked a shape per call and happened to reuse a slot.
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
    CY_CHECK_EQ(statistics->requests, 1000U);
    CY_CHECK_EQ(statistics->cache_hits, 999U);
}
