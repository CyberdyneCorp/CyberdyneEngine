// Named, counter-based random streams. Task 4.2.4.
//
// The two assertions that matter are the two the specification words as scenarios, and both are
// about *absence*: parallel sampling is safe because there is no shared state, and a new call does
// not shift the world because no other system's inputs mention it. Neither can be tested by running
// threads — the property is that there is nothing to race on — so both are tested as arithmetic.

#include <cy/core/determinism/random.h>
#include <cy/test/test.h>

#include <string_view>

namespace {

using namespace cy;
using namespace cy::determinism;

constexpr SimulationPoint kTick{Epoch{0}, 100};

}  // namespace

CY_TEST_CASE("a stream id is a compile-time constant of its name") {
    // Two spellings of one name, held in variables rather than compared inline, so that the
    // comparison is of two evaluations rather than of one expression against itself.
    constexpr const char* kSameName = "combat.crit";
    constexpr StreamId kFirst = stream_id("combat.crit");
    constexpr StreamId kSecond = stream_id(kSameName);
    static_assert(kFirst == kSecond);
    static_assert(!(kFirst == stream_id("combat.dodge")));
    static_assert(!(stream_id("ai") == substream(stream_id("ai"), 0)));

    // Two subsystems that independently name one stream get one stream, without having been
    // introduced. That is what "derived from stable identifiers" buys and it is the whole reason
    // the id is a hash of the name rather than an index into a registry.
    const RandomSource source(0x5eed);
    CY_CHECK(source.stream("ai.target").id() == source.stream(stream_id("ai.target")).id());
}

CY_TEST_CASE("a draw is a pure function of its five inputs") {
    const RandomSource source(0x5eed);
    const RandomStream stream = source.stream("combat.crit");

    const u64 first = stream.draw(kTick, 42, 0);
    CY_CHECK_EQ(stream.draw(kTick, 42, 0), first);
    CY_CHECK_NE(stream.draw(kTick, 42, 1), first);
    CY_CHECK_NE(stream.draw(kTick, 43, 0), first);
    CY_CHECK_NE(stream.draw(SimulationPoint{Epoch{0}, 101}, 42, 0), first);
    CY_CHECK_NE(stream.draw(SimulationPoint{Epoch{1}, 100}, 42, 0), first);

    // A different seed is a different session.
    CY_CHECK_NE(RandomSource(0x5eee).stream("combat.crit").draw(kTick, 42, 0), first);
}

CY_TEST_CASE("(entity + 1, tick) and (entity, tick + 1) do not collide") {
    // The reason the moment and the subject are folded separately in `draw`. A mixer that summed
    // them first would make these two equal, and the bug would look like an entity occasionally
    // seeing yesterday's roll.
    const RandomStream stream = RandomSource(1).stream("s");
    for (u64 entity = 0; entity < 64; ++entity) {
        const u64 a = stream.draw(SimulationPoint{Epoch{0}, 10}, entity + 1, 0);
        const u64 b = stream.draw(SimulationPoint{Epoch{0}, 11}, entity, 0);
        CY_REQUIRE_NE(a, b);
    }
}

CY_TEST_CASE("a new call in one system does not shift another's sequence") {
    // `simulation-and-determinism`: "WHEN a system begins consuming one more random value per tick
    // THEN other systems' sequences SHALL be unaffected."
    //
    // Testable as an identity because there is no sequence to shift: the AI stream's values do not
    // mention how many samples combat drew, and cannot, because they are not among its inputs.
    const RandomSource source(0x5eed);
    const RandomStream combat = source.stream("combat.crit");
    const RandomStream ai = source.stream("ai.target");

    u64 ai_values[8] = {};
    for (u64 index = 0; index < 8; ++index) {
        ai_values[index] = ai.draw(kTick, 7, index);
    }

    // Combat now draws four values per tick where it drew one.
    for (u64 index = 0; index < 4; ++index) {
        (void)combat.draw(kTick, 7, index);
    }

    for (u64 index = 0; index < 8; ++index) {
        CY_CHECK_EQ(ai.draw(kTick, 7, index), ai_values[index]);
    }
}

CY_TEST_CASE("below() stays in range and is not obviously biased") {
    const RandomStream stream = RandomSource(0x5eed).stream("spawn");
    constexpr u32 kBuckets = 6;
    u32 buckets[kBuckets] = {};
    for (u64 index = 0; index < 600; ++index) {
        const u32 value = stream.below(kBuckets, kTick, 0, index);
        CY_CHECK_LT(value, kBuckets);
        // Guarded rather than only checked: a `below` that broke its bound would otherwise write
        // past the array before the assertion above could report it.
        if (value < kBuckets) {
            ++buckets[value];
        }
    }
    // A uniform draw puts 100 in each bucket. The bound is loose on purpose: this is a smoke test
    // that the multiply-shift is not producing a constant, not a statistical claim.
    for (const u32 count : buckets) {
        CY_CHECK_GT(count, 50U);
        CY_CHECK_LT(count, 160U);
    }

    CY_CHECK_EQ(stream.below(1, kTick, 0, 0), 0U);
    CY_CHECK_EQ(stream.below(0, kTick, 0, 0), 0U);
}

CY_TEST_CASE("unit_float and unit_double are in [0, 1)") {
    const RandomStream stream = RandomSource(9).stream("jitter");
    for (u64 index = 0; index < 256; ++index) {
        const f32 single = stream.unit_float(kTick, index, 0);
        const f64 wide = stream.unit_double(kTick, index, 0);
        CY_CHECK_GE(single, 0.0F);
        CY_CHECK_LT(single, 1.0F);
        CY_CHECK_GE(wide, 0.0);
        CY_CHECK_LT(wide, 1.0);
    }
}

CY_TEST_CASE("a presentation stream says so") {
    // `simulation-and-determinism`: "Streams used only for presentation SHALL be declared as such
    // and SHALL NOT be required to be reproducible." The declaration is on the stream so that the
    // validator can ignore a muzzle flash and cannot ignore a critical hit.
    const RandomSource source(1);
    CY_CHECK(source.stream("combat.crit").authoritative());
    CY_CHECK_FALSE(source.stream("vfx.spark", StreamPurpose::Presentation).authoritative());

    // Purpose does not change the values: a stream that became presentation-only would otherwise
    // silently change every draw it had already made.
    CY_CHECK_EQ(source.stream("x").draw(kTick, 0, 0),
                source.stream("x", StreamPurpose::Presentation).draw(kTick, 0, 0));
}

CY_TEST_CASE("a sample cursor is local and carries no ordering meaning") {
    const RandomStream stream = RandomSource(3).stream("loot");
    SampleCursor first;
    SampleCursor second;
    // Two cursors over one stream at one moment for one entity give the same values, which is
    // correct: they are the same samples. A cursor is a spelling convenience, not state.
    for (u32 i = 0; i < 4; ++i) {
        CY_CHECK_EQ(stream.draw(kTick, 1, first.next()), stream.draw(kTick, 1, second.next()));
    }
    CY_CHECK_EQ(first.drawn(), 4ULL);
}
