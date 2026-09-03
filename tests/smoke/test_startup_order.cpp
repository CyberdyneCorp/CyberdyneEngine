// Startup and shutdown order, across a hundred processes. Task 3.4.4.
//
// `engine-architecture` requires the runtime to initialise subsystems in a fixed order and tear
// them down in exact reverse. "Fixed" is a claim about every run, so this runs the probe a hundred
// times, in a hundred processes, and requires all hundred to print the same two lines.
//
// Separate processes rather than a loop inside one: a fresh address space each time is what would
// expose an order that depended on an allocation address, a hash seed or the order of static
// initialisers. A hundred iterations of one loop would reproduce such an order faithfully and
// report it as deterministic.

#include <cy/test/test.h>

#include <string>

#include "process.h"

namespace {

constexpr int kRuns = 100;

// The order the specification fixes, as text, so that a reordering fails with the two sequences
// side by side rather than with "run 37 differs from run 1".
constexpr const char* kExpected =
    "startup  platform core modules-core display servers modules-servers ecs-scene modules-scene "
    "scripting editor boot\n"
    "shutdown boot editor scripting modules-scene ecs-scene modules-servers servers display "
    "modules-core core platform\n";

}  // namespace

CY_TEST_CASE("startup and shutdown order is identical across 100 runs") {
    const std::string command = cy::test::smoke::quoted(CY_STARTUP_PROBE);

    const cy::test::smoke::ProcessResult first = cy::test::smoke::run(command);
    CY_REQUIRE(first.ran);
    CY_REQUIRE_EQ(first.exit_code, 0);
    CY_CHECK_EQ(first.output, std::string{kExpected});

    int identical = 1;
    for (int run = 1; run < kRuns; ++run) {
        const cy::test::smoke::ProcessResult next = cy::test::smoke::run(command);
        CY_REQUIRE(next.ran);
        CY_REQUIRE_EQ(next.exit_code, 0);
        if (next.output == first.output) {
            ++identical;
        } else {
            CY_TEST_FAIL_CHECK("run " << run << " differs.\n  first: " << first.output
                                      << "  this:  " << next.output);
        }
    }

    CY_CHECK_EQ(identical, kRuns);
    CY_TEST_MESSAGE(identical << " of " << kRuns << " runs produced the same order");
}
