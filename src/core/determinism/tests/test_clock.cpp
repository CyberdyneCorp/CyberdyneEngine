// The simulation clock and epochs. Tasks 4.2.1 and 4.2.2.
//
// The interesting assertions here are the two the specification words as scenarios: a session that
// runs for hours does not drift, and a long frame runs more ticks rather than a longer one. Both
// are checked against the accumulator's arithmetic rather than against a wall clock — this class
// cannot read one, which is the point.

#include <cy/core/determinism/clock.h>
#include <cy/test/test.h>

#include <string_view>

namespace {

using namespace cy;
using namespace cy::determinism;

constexpr Nanoseconds kSixtyHzFrame = 16'666'667;  // the rounded step a naive loop would use

SimulationClock make_clock(TickRate rate, u32 cap = 8) {
    SimulationClock clock;
    ClockConfig config;
    config.rate = rate;
    config.max_ticks_per_frame = cap;
    CY_REQUIRE(static_cast<bool>(clock.configure(config)));
    return clock;
}

}  // namespace

CY_TEST_CASE("a tick rate is an exact rational and an invalid one is refused") {
    CY_CHECK(static_cast<bool>(validate(TickRate{60, 1})));
    CY_CHECK(static_cast<bool>(validate(TickRate{30000, 1001})));
    CY_CHECK_FALSE(static_cast<bool>(validate(TickRate{0, 1})));
    CY_CHECK_FALSE(static_cast<bool>(validate(TickRate{60, 0})));
    CY_CHECK_FALSE(static_cast<bool>(validate(TickRate{kMaxTicksPerSecond + 1, 1})));
    CY_CHECK_FALSE(static_cast<bool>(validate(TickRate{kMaxRateTerm + 1, kMaxRateTerm + 1})));
    // 30000/1001 is 29.97 ticks per second: large terms, a modest rate. The two bounds are separate
    // so that this is legal and 20 000 ticks per second is not.
    CY_CHECK(static_cast<bool>(validate(TickRate{30000, 1001})));

    // The rounded step is what a nanosecond-based loop would accumulate, and it is wrong by a third
    // of a nanosecond per tick. It is still the right answer for a *duration*, which is all
    // step_nanoseconds() claims to give.
    CY_CHECK_EQ(step_nanoseconds(TickRate{60, 1}), 16'666'666);
}

CY_TEST_CASE("two minutes of ticks do not drift") {
    // `simulation-and-determinism`: "WHEN a session runs for hours at 60 ticks per second THEN tick
    // timing SHALL be derived from an exact rational rate rather than accumulated floating-point
    // steps."
    //
    // The test feeds exactly one second of nanoseconds per iteration and asserts the tick count is
    // exactly 60 every time. With a step of 16'666'666 ns the 60th tick would fall short by 40 ns
    // per second, and after 25 seconds a whole extra tick would appear in one of the frames — so
    // two minutes is already enough to falsify a drifting implementation several times over. The
    // *hours* the scenario names are in `integration.determinism_scale`, because 216 000 calls is
    // over the unit suite's millisecond in a Debug build.
    SimulationClock clock = make_clock(TickRate{60, 1}, 64);
    for (u32 second = 0; second < 120; ++second) {
        const FrameTicks ticks = clock.accumulate(1'000'000'000);
        CY_REQUIRE_EQ(ticks.ticks, 60U);
        for (u32 i = 0; i < ticks.ticks; ++i) {
            clock.advance();
        }
        CY_REQUIRE_EQ(clock.interpolation_alpha(), 0.0F);
    }
    CY_CHECK_EQ(clock.tick(), 120ULL * 60ULL);
    CY_CHECK_EQ(clock.seconds(), 120.0);
    CY_CHECK_EQ(clock.discarded_ns(), 0ULL);
}

CY_TEST_CASE("a long frame runs more ticks and never a longer one") {
    // "WHEN a frame takes 50 milliseconds THEN the simulation SHALL execute additional fixed ticks
    // up to the configured bound, and the step SHALL remain unchanged."
    SimulationClock clock = make_clock(TickRate{60, 1});
    const f32 step_before = clock.delta_seconds();

    const FrameTicks ticks = clock.accumulate(50'000'000);
    CY_CHECK_EQ(ticks.ticks, 3U);
    CY_CHECK_EQ(clock.delta_seconds(), step_before);
    CY_CHECK_EQ(clock.discarded_ns(), 0ULL);
}

CY_TEST_CASE("a slow frame is bounded and the excess is counted, not chased") {
    // `engine-architecture`: "WHEN a frame takes 400 ms THEN at most max_ticks_per_frame simulation
    // ticks SHALL run and remaining accumulated time SHALL be discarded with a diagnostic counter
    // incremented."
    SimulationClock clock = make_clock(TickRate{60, 1}, 8);
    const FrameTicks ticks = clock.accumulate(400'000'000);
    CY_REQUIRE_EQ(ticks.ticks, 8U);
    CY_CHECK_EQ(clock.capped_frames(), 1ULL);
    CY_CHECK_GT(clock.discarded_ns(), 0ULL);

    // 400 ms is 24 steps; eight ran, sixteen were dropped. The residue that becomes the alpha is
    // kept, which is why the discarded figure is the whole steps and not the whole remainder.
    CY_CHECK_EQ(clock.discarded_ns(), (16ULL * 16'666'666ULL) + 10ULL);

    for (u32 i = 0; i < ticks.ticks; ++i) {
        clock.advance();
    }
    // The next frame starts from the residue, not from a backlog: the death spiral is closed.
    const FrameTicks next = clock.accumulate(0);
    CY_CHECK_EQ(next.ticks, 0U);
}

CY_TEST_CASE("the interpolation alpha is the exact residue") {
    SimulationClock clock = make_clock(TickRate{60, 1});
    // Half a step.
    const FrameTicks ticks = clock.accumulate(kSixtyHzFrame / 2);
    CY_CHECK_EQ(ticks.ticks, 0U);
    CY_CHECK_GT(clock.interpolation_alpha(), 0.49F);
    CY_CHECK_LT(clock.interpolation_alpha(), 0.51F);

    // Two and a half steps: two ticks, and the alpha is unchanged by consuming them.
    const FrameTicks more = clock.accumulate(2 * kSixtyHzFrame);
    CY_REQUIRE_EQ(more.ticks, 2U);
    const f32 before = clock.interpolation_alpha();
    clock.advance();
    clock.advance();
    CY_CHECK_EQ(clock.interpolation_alpha(), before);
}

CY_TEST_CASE("fixed-step mode ignores the wall clock entirely") {
    // `engine-architecture`'s `--fixed-step <n>`: "exactly one tick of fixed duration SHALL run per
    // frame regardless of wall-clock time, producing reproducible simulation for recording and
    // automated tests."
    SimulationClock clock;
    ClockConfig config;
    config.mode = TickMode::FixedStep;
    config.fixed_ticks_per_frame = 1;
    CY_REQUIRE(static_cast<bool>(clock.configure(config)));

    for (const Nanoseconds elapsed : {Nanoseconds{0}, Nanoseconds{1}, Nanoseconds{9'999'999'999}}) {
        const FrameTicks ticks = clock.accumulate(elapsed);
        CY_CHECK_EQ(ticks.ticks, 1U);
        CY_CHECK_EQ(ticks.alpha, 0.0F);
        clock.advance();
    }
    CY_CHECK_EQ(clock.tick(), 3ULL);
    CY_CHECK_EQ(clock.discarded_ns(), 0ULL);
}

CY_TEST_CASE("fixed-step mode is not an exemption from the tick cap") {
    SimulationClock clock;
    ClockConfig config;
    config.mode = TickMode::FixedStep;
    config.max_ticks_per_frame = 4;
    config.fixed_ticks_per_frame = 5;
    CY_CHECK_FALSE(static_cast<bool>(clock.configure(config)));
}

CY_TEST_CASE("the same tick in two epochs is two moments") {
    // `simulation-and-determinism`: "WHEN a rollback returns to a tick already simulated THEN the
    // resulting state SHALL be identified by a distinct simulation point."
    SimulationClock clock = make_clock(TickRate{60, 1});
    for (u32 i = 0; i < 100; ++i) {
        (void)clock.accumulate(kSixtyHzFrame);
        clock.advance();
    }
    const SimulationPoint before = clock.now();
    CY_REQUIRE_EQ(before.tick, 100ULL);

    const Epoch after_reset = clock.reset(EpochReason::CheckpointRestore, 100);
    const SimulationPoint after = clock.now();

    CY_CHECK_EQ(after.tick, before.tick);
    CY_CHECK_NE(after, before);
    CY_CHECK_EQ(after_reset.value, before.epoch.value + 1);
    CY_CHECK(clock.epochs().reason() == EpochReason::CheckpointRestore);
}

CY_TEST_CASE("a cache stamped in an old epoch is stale") {
    SimulationClock clock = make_clock(TickRate{60, 1});
    const SimulationPoint stamp = clock.now();
    CY_CHECK_FALSE(is_stale(stamp, clock.now()));

    (void)clock.reset(EpochReason::WorldReload, 0);
    CY_CHECK(is_stale(stamp, clock.now()));

    // Within one epoch, a stamp from the future is stale too — that is a rollback that forgot to
    // bump the epoch, and it is the case a plain epoch comparison would miss.
    const SimulationPoint ahead{clock.epoch(), 500};
    CY_CHECK(is_stale(ahead, clock.now()));
}

CY_TEST_CASE("resuming a recorded session continues its timeline") {
    SimulationClock clock = make_clock(TickRate{60, 1});
    clock.resume(SimulationPoint{Epoch{7}, 41200}, EpochReason::SessionRestart);
    CY_CHECK_EQ(clock.epoch().value, 7U);
    CY_CHECK_EQ(clock.tick(), 41200ULL);
    CY_CHECK_EQ(clock.interpolation_alpha(), 0.0F);
}
