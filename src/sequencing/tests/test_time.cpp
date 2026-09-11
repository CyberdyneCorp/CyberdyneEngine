// Exact time. M8.c task 3.2, and every case here is a sentence of the "Exact time" requirement.

#include <cy/sequencing/time.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::sequencing;

CY_TEST_CASE("sequence_time: a frame and a subframe, not an accumulated float") {
    const SequenceTime time = SequenceTime::from_frame(12, 250);
    CY_CHECK_EQ(time.frame(), 12);
    CY_CHECK_EQ(time.subframe(), 250);
    CY_CHECK_EQ(time.ticks(), (12 * kTicksPerFrame) + 250);

    // Floored, so the frame before zero is -1 rather than 0. A pre-roll finds a truncating
    // division; nothing else does.
    const SequenceTime before = SequenceTime::from_ticks(-1);
    CY_CHECK_EQ(before.frame(), -1);
    CY_CHECK_EQ(before.subframe(), kTicksPerFrame - 1);
}

CY_TEST_CASE("sequence_time: a rate outside the representable range is refused") {
    TimeAccumulator accumulator;
    CY_CHECK_FALSE(accumulator.advance(Rate{0, 1}, PlayRate::normal(), 1000));
    CY_CHECK_FALSE(accumulator.advance(Rate{24, 0}, PlayRate::normal(), 1000));
    CY_CHECK_FALSE(accumulator.advance(Rate{24, 1}, PlayRate{1, 0}, 1000));
    // A single advance longer than a second: refused rather than overflowed. See time.cpp for the
    // worst case this bound protects.
    CY_CHECK_FALSE(
        accumulator.advance(Rate{24, 1}, PlayRate::normal(), kMaxAdvanceNanoseconds + 1));
}

CY_TEST_CASE("sequence_time: one frame of wall time is exactly one frame") {
    TimeAccumulator accumulator;
    const Rate rate{24, 1};
    // 1/24 s in nanoseconds is 41,666,666.67 — not an integer, which is the point: the remainder is
    // carried, so 24 of them are exactly 24 frames rather than 23.999.
    const Expected<SequenceTime, Error> first =
        accumulator.advance(rate, PlayRate::normal(), 41666667);
    CY_REQUIRE(first);
    CY_CHECK_EQ(first.value().frame(), 1);
}

CY_TEST_CASE("sequence_time: arriving by any route lands on the same instant") {
    // "**WHEN** a time is reached by playing and by seeking **THEN** the evaluated result SHALL be
    // identical." Here at the level time itself: a hundred small advances and one large one.
    const Rate rate{30000, 1001};  // 29.97
    TimeAccumulator played;
    i64 by_playing = 0;
    for (i32 index = 0; index < 100; ++index) {
        const Expected<SequenceTime, Error> delta =
            played.advance(rate, PlayRate::normal(), 10000000);
        CY_REQUIRE(delta);
        by_playing += delta.value().ticks();
    }
    TimeAccumulator once;
    const Expected<SequenceTime, Error> whole = once.advance(rate, PlayRate::normal(), 1000000000);
    CY_REQUIRE(whole);
    CY_CHECK_EQ(by_playing, whole.value().ticks());
}

CY_TEST_CASE("sequence_time: half speed is exactly one half") {
    TimeAccumulator full;
    TimeAccumulator half;
    const Rate rate{24, 1};
    i64 at_full = 0;
    i64 at_half = 0;
    for (i32 index = 0; index < 50; ++index) {
        at_full += full.advance(rate, PlayRate::normal(), 20000000).value().ticks();
        at_half += half.advance(rate, PlayRate::half(), 20000000).value().ticks();
    }
    CY_CHECK_EQ(at_full, at_half * 2);
}

CY_TEST_CASE("sequence_time: reverse playback returns to the instant it left") {
    // Reverse playback is the negative rate, and what has to hold is that going back over the same
    // wall time lands on the same instant — WITH THE SAME ACCUMULATOR, which is what a sequence
    // played backwards actually does.
    //
    // The rounding is FLOORED, so a separate accumulator run backwards is NOT the negation of one
    // run forwards: floor(-x) is -ceil(x), and the two differ by a tick whenever the division is
    // inexact. That is a deliberate consequence of flooring — the alternative, truncating toward
    // zero, makes the instant before zero and the instant after zero both round to zero — and it is
    // written here rather than discovered later.
    TimeAccumulator accumulator;
    const Rate rate{24, 1};
    i64 ticks = 0;
    for (i32 index = 0; index < 37; ++index) {
        ticks += accumulator.advance(rate, PlayRate::normal(), 12345678).value().ticks();
    }
    CY_CHECK_GT(ticks, 0);
    for (i32 index = 0; index < 37; ++index) {
        ticks += accumulator.advance(rate, PlayRate{-1, 1}, 12345678).value().ticks();
    }
    CY_CHECK_EQ(ticks, 0);
    CY_CHECK_EQ(accumulator.residual(), 0);
}

CY_TEST_CASE("sequence_time: only the simulation domain carries authoritative gameplay") {
    CY_CHECK(domain_permits_authoritative(ClockDomain::Simulation));
    CY_CHECK_FALSE(domain_permits_authoritative(ClockDomain::Presentation));
    CY_CHECK_FALSE(domain_permits_authoritative(ClockDomain::Cinematic));
    CY_CHECK_FALSE(domain_permits_authoritative(ClockDomain::RealTime));
    CY_CHECK_FALSE(domain_permits_authoritative(ClockDomain::External));
}
