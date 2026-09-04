// The cases whose subject is a length of time rather than a behaviour. Task 4.2.1.
//
// Integration and not unit for one reason, and it is cost: `simulation-and-determinism`'s no-drift
// scenario is a session that runs "for hours", which is a quarter of a million clock calls, and the
// unit suite's budget is one millisecond in every profile including Debug. The behaviour these
// cases assert is already covered in `unit.determinism` over two minutes; what they add is the
// length the specification actually names.

#include <cy/core/determinism/clock.h>
#include <cy/test/test.h>

#include <string_view>

namespace {

using namespace cy;
using namespace cy::determinism;

SimulationClock make_clock(TickRate rate) {
    SimulationClock clock;
    ClockConfig config;
    config.rate = rate;
    config.max_ticks_per_frame = 64;
    CY_REQUIRE(static_cast<bool>(clock.configure(config)));
    return clock;
}

}  // namespace

CY_TEST_CASE("a session that runs for hours does not drift") {
    SimulationClock clock = make_clock(TickRate{60, 1});
    for (u32 second = 0; second < 4 * 3600; ++second) {
        const FrameTicks ticks = clock.accumulate(1'000'000'000);
        CY_REQUIRE_EQ(ticks.ticks, 60U);
        for (u32 i = 0; i < ticks.ticks; ++i) {
            clock.advance();
        }
    }
    CY_CHECK_EQ(clock.tick(), 4ULL * 3600ULL * 60ULL);
    CY_CHECK_EQ(clock.seconds(), 4.0 * 3600.0);
    CY_CHECK_EQ(clock.interpolation_alpha(), 0.0F);
    CY_CHECK_EQ(clock.discarded_ns(), 0ULL);
}

CY_TEST_CASE("a rate that is not a whole number of ticks per second is still exact") {
    // 30000/1001 is 29.97 ticks per second — the rate that exists precisely because it cannot be
    // written as a step. One thousand and one seconds is exactly 30 000 ticks, and no accumulator
    // holding a rounded nanosecond step reaches that number.
    SimulationClock clock = make_clock(TickRate{30000, 1001});
    for (u32 second = 0; second < 1001; ++second) {
        const FrameTicks ticks = clock.accumulate(1'000'000'000);
        for (u32 i = 0; i < ticks.ticks; ++i) {
            clock.advance();
        }
    }
    CY_CHECK_EQ(clock.tick(), 30000ULL);
    CY_CHECK_EQ(clock.seconds(), 1001.0);
    CY_CHECK_EQ(clock.interpolation_alpha(), 0.0F);
}

CY_TEST_CASE("a frame that misses badly does not build a backlog") {
    // Two hundred frames that each take a quarter of a second, at a cap of eight. Every frame runs
    // exactly eight ticks and the accumulator never grows, which is the whole content of "the loop
    // cannot enter a death spiral".
    SimulationClock clock;
    ClockConfig config;
    config.max_ticks_per_frame = 8;
    CY_REQUIRE(static_cast<bool>(clock.configure(config)));

    for (u32 frame = 0; frame < 200; ++frame) {
        const FrameTicks ticks = clock.accumulate(250'000'000);
        CY_REQUIRE_EQ(ticks.ticks, 8U);
        for (u32 i = 0; i < ticks.ticks; ++i) {
            clock.advance();
        }
    }
    CY_CHECK_EQ(clock.tick(), 200ULL * 8ULL);
    CY_CHECK_EQ(clock.capped_frames(), 200ULL);
    // Recovery is immediate: a frame of nothing buys no ticks, so nothing was being carried.
    CY_CHECK_EQ(clock.accumulate(0).ticks, 0U);
}
