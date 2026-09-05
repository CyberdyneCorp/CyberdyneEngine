// TASK 4.4.5 — deterministic random streams over M2's seeded streams.
//
// The property to protect is **independence**. `gameplay-framework`: "Streams SHALL be independent,
// so that consuming randomness in one system does not perturb another's sequence — which is what
// makes a change in one feature alter unrelated outcomes in replay."
//
// The first case is the one that would fail against a shared generator, and it is written as the
// change that causes the failure: combat draws more samples than it used to, and loot must not
// notice. With a single generator every loot outcome after the change shifts, and a replay recorded
// before it diverges for a reason that has nothing to do with loot — a bug that is nearly
// impossible to attribute after the fact.

#include "fixture.h"

using namespace cy::gameplay_test;
using cy::u32;
using cy::u64;

namespace {

/// A moment. Every draw is a pure function of (seed, stream, epoch, tick, subject, index), so a
/// case states its moment rather than advancing a hidden counter.
[[nodiscard]] cy::determinism::SimulationPoint at(u64 tick) noexcept {
    cy::determinism::SimulationPoint point;
    point.tick = tick;
    return point;
}

}  // namespace

CY_TEST_CASE("gameplay: consuming more randomness in one stream does not perturb another") {
    const GameplayRandom random(0xA11CEULL);
    const RandomStream combat = random.stream("combat.crit");
    const RandomStream loot = random.stream("loot.drop");

    // Loot's sequence, sampled before anything changes.
    u32 before[8];
    for (u32 index = 0; index < 8; ++index) {
        before[index] = loot.below(100, at(10), 0, index);
    }

    // Combat now draws twelve samples where it used to draw three — the ordinary shape of a
    // gameplay change. With a shared generator this shifts every subsequent loot value.
    for (u32 index = 0; index < 12; ++index) {
        (void)combat.below(20, at(10), 0, index);
    }

    for (u32 index = 0; index < 8; ++index) {
        CY_CHECK_EQ(loot.below(100, at(10), 0, index), before[index]);
    }
}

CY_TEST_CASE("gameplay: a session replays its random outcomes from the seed") {
    // Same seed, same stream, same moment, same subject: same value. That is the whole of
    // "Reproducible session", and it holds without any state being carried between the two.
    const GameplayRandom first(0xBEEFULL);
    const GameplayRandom second(0xBEEFULL);
    for (u64 tick = 1; tick <= 16; ++tick) {
        for (u32 subject = 0; subject < 4; ++subject) {
            CY_CHECK_EQ(first.stream("combat.crit").draw(at(tick), subject, 0),
                        second.stream("combat.crit").draw(at(tick), subject, 0));
        }
    }

    // A different session seed gives a different sequence, or the reproducibility above would be
    // reproducing a constant.
    const GameplayRandom other(0xBEEFULL + 1);
    CY_CHECK_NE(first.stream("combat.crit").draw(at(1), 0, 0),
                other.stream("combat.crit").draw(at(1), 0, 0));
}

CY_TEST_CASE("gameplay: two participants draw independent sequences from one stream") {
    Fixture fixture;
    auto alice =
        fixture.session.add_participant(ParticipantKind::LocalHuman, cy::Name::intern("alice"));
    auto bob =
        fixture.session.add_participant(ParticipantKind::RemoteHuman, cy::Name::intern("bob"));
    CY_REQUIRE(alice.has_value());
    CY_REQUIRE(bob.has_value());

    // Constructed from the session, so there is no path to a stream that is not the session's.
    const GameplayRandom random(fixture.session);
    CY_CHECK_EQ(random.seed(), fixture.session.seed());

    const StreamId parent = cy::determinism::stream_id("loot.drop");
    const RandomStream for_alice = random.per_participant(parent, *alice);
    const RandomStream for_bob = random.per_participant(parent, *bob);
    CY_CHECK_NE(for_alice.id().value, for_bob.id().value);

    u32 differences = 0;
    for (u32 index = 0; index < 32; ++index) {
        differences +=
            for_alice.below(1000, at(5), 0, index) != for_bob.below(1000, at(5), 0, index) ? 1U
                                                                                           : 0U;
    }
    // Not "all different" — two independent sequences over 1000 values collide sometimes, and a
    // test that demanded otherwise would be a test that fails on a seed change. Most different is
    // the honest assertion.
    CY_CHECK_GT(differences, 28);
}

CY_TEST_CASE("gameplay: a presentation stream is declared as one") {
    const GameplayRandom random(0x5EEDULL);
    const RandomStream jitter = random.presentation("muzzle.jitter");
    const RandomStream crit = random.stream("combat.crit");

    // `core-determinism`: the classification is on the *stream*, not on the draw, "because a stream
    // that is authoritative on Tuesday and presentation on Wednesday is the bug this classification
    // exists to name". A validator ignores the first and cannot ignore the second.
    CY_CHECK_FALSE(jitter.authoritative());
    CY_CHECK(crit.authoritative());
    CY_CHECK_EQ(jitter.purpose(), StreamPurpose::Presentation);
}

CY_TEST_CASE("gameplay: a keyed substream is hierarchical identity without string concatenation") {
    const GameplayRandom random(0x5EEDULL);
    const StreamId abilities = cy::determinism::stream_id("ability");
    const RandomStream first = random.keyed(abilities, 1);
    const RandomStream second = random.keyed(abilities, 2);
    CY_CHECK_NE(first.id().value, second.id().value);
    // And the derivation is stable: asking twice gives the same stream, so nothing has to be
    // registered or remembered.
    CY_CHECK_EQ(random.keyed(abilities, 1).id().value, first.id().value);
}

CY_TEST_CASE("gameplay: sampling is parallel-safe because it carries no state") {
    const GameplayRandom random(0x5EEDULL);
    const RandomStream stream = random.stream("spawn.scatter");

    // Two cursors over one stream at one moment for one subject produce the same values — which is
    // correct: they are the same samples. There is no shared generator to contend for and no
    // ordering dependency to get wrong, which is what makes many entities sampling at once safe by
    // construction rather than by a lock.
    SampleCursor first;
    SampleCursor second;
    for (u32 index = 0; index < 8; ++index) {
        CY_CHECK_EQ(stream.unit_float(at(3), 7, first.next()),
                    stream.unit_float(at(3), 7, second.next()));
    }
    CY_CHECK_EQ(first.drawn(), 8);
}
