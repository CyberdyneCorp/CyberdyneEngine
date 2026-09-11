// M9 TASK 1.4 — THE SIDE-EFFECT LEDGER, AND THE DUPLICATE-EFFECT PROOF.
//
// ================================================================================================
// THE FIRST CASE IS THE EXIT CRITERION'S OWN TEST AND IT IS A NEGATIVE CONTROL
// ================================================================================================
//
// `docs/ROADMAP.md` M9: "Rollback re-simulates without re-applying ledgered side effects, **proven
// by a duplicate-effect test**", and `replay-and-rollback`'s ADDED requirement says what "proven"
// has to mean here: "The test that proves it SHALL fail when the ledger is removed, and that SHALL
// be demonstrated rather than asserted — for the same reason `vfx-system`'s firewall delta requires
// it at M8.c: a test that still passes when its subject is deleted is a test of nothing."
//
// THE MUTATION. In `SideEffectLedger::realise()` (src/replay/src/ledger.cpp), replace the body's
// duplicate check with an unconditional `return EffectVerdict::Realise;`. The first case below goes
// red on `explosions_played == 1`. The run is pasted into src/replay/README.md.
//
// ================================================================================================
// AND THE KEY IS THE TICK, NOT THE (EPOCH, TICK) PAIR
// ================================================================================================
//
// A rollback restores a checkpoint, which advances the epoch. The second case re-simulates the same
// tick in a NEW epoch, which is what actually happens, and it is the case a pair-keyed ledger would
// fail — silently, by suppressing nothing and looking perfectly correct in every other test.

#include "fixture.h"

#include <cy/replay/ledger.h>

using namespace cy::replay_test;
using cy::u32;
using cy::u64;
using cy::determinism::Epoch;
using cy::determinism::SimulationPoint;

namespace {

constexpr u64 kExplosion = 0xE'7107;
constexpr u64 kAchievement = 0xACC'01AD;
constexpr u64 kMuzzleFlash = 0xF1A54;

/// Fills a ledger rather than returning one: `SideEffectLedger` is non-copyable and deliberately
/// non-movable, because a ledger that could be moved out from under a running session is a ledger
/// whose suppression window silently restarts.
void declare_effects(SideEffectLedger& ledger) noexcept {
    CY_REQUIRE(
        ledger
            .declare({kExplosion, "explosion", Speculation::ConfirmedOnly, Reconciliation::Cancel})
            .has_value());
    CY_REQUIRE(ledger
                   .declare({kAchievement, "achievement", Speculation::ConfirmedOnly,
                             Reconciliation::Cancel})
                   .has_value());
    CY_REQUIRE(ledger
                   .declare({kMuzzleFlash, "muzzle flash", Speculation::Speculative,
                             Reconciliation::AllowToFinish})
                   .has_value());
}

}  // namespace

CY_TEST_CASE("replay: re-simulating a tick does not play the explosion twice") {
    SideEffectLedger ledger(allocator());
    declare_effects(ledger);
    u32 explosions_played = 0;

    // A tiny simulation of one tick. The instance is a function of simulation state — the entity
    // that exploded — and not a counter, which is what makes the second run ask about the same
    // effect. `realise()` cannot check that, and the third case below shows what a counter does.
    const u64 exploding_entity = 77;
    auto simulate_tick_41 = [&](Epoch epoch) noexcept {
        const SimulationPoint at{epoch, 41};
        if (ledger.realise(kExplosion, exploding_entity, at, false) == EffectVerdict::Realise) {
            ++explosions_played;
        }
    };

    simulate_tick_41(Epoch{1});
    CY_REQUIRE_EQ(explosions_played, 1U);

    // The rollback: restore the checkpoint, advance the epoch, re-simulate.
    simulate_tick_41(Epoch{2});
    simulate_tick_41(Epoch{3});

    // "WHEN a tick that triggered an explosion is re-simulated after a rollback THEN the effect
    // SHALL be suppressed rather than triggered again."
    CY_CHECK_EQ(explosions_played, 1U);
    CY_CHECK_EQ(ledger.report().realised, 1U);
    CY_CHECK_EQ(ledger.report().suppressed, 2U);
    // The epoch it was FIRST realised in is kept, so a crash artefact can say "first realised in
    // epoch 1, suppressed in epochs 2 and 3".
    CY_REQUIRE_EQ(ledger.size(), 1U);
    CY_CHECK_EQ(ledger.at(0).first_epoch.value, 1U);
}

CY_TEST_CASE("replay: a different tick, entity or effect kind is a different effect") {
    // The control for the case above. A ledger that suppressed everything would pass it, and that
    // is the same defect as one that suppresses nothing.
    SideEffectLedger ledger(allocator());
    declare_effects(ledger);
    const SimulationPoint at{Epoch{1}, 41};
    CY_CHECK(ledger.realise(kExplosion, 77, at, false) == EffectVerdict::Realise);
    CY_CHECK(ledger.realise(kExplosion, 78, at, false) == EffectVerdict::Realise);
    CY_CHECK(ledger.realise(kExplosion, 77, SimulationPoint{Epoch{1}, 42}, false) ==
             EffectVerdict::Realise);
    CY_CHECK(ledger.realise(kMuzzleFlash, 77, at, false) == EffectVerdict::Realise);
    CY_CHECK_EQ(ledger.report().realised, 4U);
    CY_CHECK_EQ(ledger.report().suppressed, 0U);
}

CY_TEST_CASE("replay: an instance from a session counter suppresses nothing, and it is visible") {
    // The header warns that `instance` must be a function of simulation state rather than of a
    // counter. This case demonstrates the failure rather than leaving the warning to be believed.
    //
    // The counter that breaks it is a SESSION-lived one — the natural thing to write, an id issued
    // per effect as the session runs — because a re-simulation keeps counting from where the first
    // simulation left off and therefore asks the ledger about effects it has never heard of. (A
    // counter reset per tick happens to work, and it works by accident: it is a function of the
    // tick, which is what a correct key is.)
    SideEffectLedger ledger(allocator());
    declare_effects(ledger);
    u32 played = 0;
    u64 counter = 0;  // lives across the re-simulation — the mistake
    auto simulate_with_counter = [&](Epoch epoch) noexcept {
        for (u32 index = 0; index < 3; ++index) {
            if (ledger.realise(kExplosion, 0x1000'0000ULL + counter++, SimulationPoint{epoch, 41},
                               false) == EffectVerdict::Realise) {
                ++played;
            }
        }
    };
    simulate_with_counter(Epoch{1});
    simulate_with_counter(Epoch{2});
    // Three effects the first time and three *more* the second, because the second run invented new
    // identities. The ledger did its job; the caller did not give it a key it could match on.
    CY_CHECK_EQ(played, 6U);
    CY_CHECK_EQ(ledger.report().suppressed, 0U);

    // And the control: the same loop keyed on the entity that exploded — a function of simulation
    // state — suppresses every re-simulated effect.
    SideEffectLedger keyed(allocator());
    declare_effects(keyed);
    u32 keyed_played = 0;
    auto simulate_keyed = [&](Epoch epoch) noexcept {
        for (u64 entity = 90; entity < 93; ++entity) {
            if (keyed.realise(kExplosion, entity, SimulationPoint{epoch, 41}, false) ==
                EffectVerdict::Realise) {
                ++keyed_played;
            }
        }
    };
    simulate_keyed(Epoch{1});
    simulate_keyed(Epoch{2});
    CY_CHECK_EQ(keyed_played, 3U);
    CY_CHECK_EQ(keyed.report().suppressed, 3U);
}

CY_TEST_CASE("replay: an achievement waits for the authority and a muzzle flash does not") {
    // "A muzzle flash may be speculative; an achievement, a currency change, or a persistent unlock
    // SHALL NOT be."
    SideEffectLedger ledger(allocator());
    declare_effects(ledger);
    const SimulationPoint at{Epoch{1}, 10};

    CY_CHECK(ledger.realise(kMuzzleFlash, 1, at, true) == EffectVerdict::Realise);
    CY_CHECK(ledger.realise(kAchievement, 1, at, true) == EffectVerdict::DeferredUntilConfirmed);
    CY_CHECK_EQ(ledger.report().deferred, 1U);

    // Held, not dropped. When the authority confirms the tick it becomes realisable.
    CY_CHECK_EQ(ledger.confirm(10), 2U);
    CY_CHECK(ledger.realise(kAchievement, 1, SimulationPoint{Epoch{2}, 10}, false) ==
             EffectVerdict::SuppressedAlreadyRealised);

    // An undeclared kind is refused rather than assumed speculative: an undeclared achievement
    // would otherwise fire on a prediction.
    CY_CHECK(ledger.realise(0x0BAD'0BADULL, 1, at, true) == EffectVerdict::Undeclared);
    CY_CHECK_EQ(ledger.report().undeclared, 1U);
}

CY_TEST_CASE("replay: an invalidated effect gets its kind's declared reconciliation") {
    // "Effects that were realised on a timeline that re-simulation has invalidated SHALL be
    // reconcilable: cancelled, allowed to finish, or corrected, by declared policy per effect
    // kind." Per kind, so two kinds invalidated at the same tick get different answers.
    SideEffectLedger ledger(allocator());
    declare_effects(ledger);
    const SimulationPoint at{Epoch{1}, 20};
    CY_CHECK(ledger.realise(kExplosion, 5, at, false) == EffectVerdict::Realise);
    CY_CHECK(ledger.realise(kMuzzleFlash, 5, at, false) == EffectVerdict::Realise);

    CY_CHECK(ledger.invalidate(kExplosion, 5, 20) == Reconciliation::Cancel);
    CY_CHECK(ledger.invalidate(kMuzzleFlash, 5, 20) == Reconciliation::AllowToFinish);
    CY_CHECK_EQ(ledger.report().invalidated, 2U);
}

CY_TEST_CASE("replay: the ledger is bounded, pruned by the window, and says what it forgot") {
    SideEffectLedger ledger(allocator(), 16);
    CY_REQUIRE(
        ledger.declare({kExplosion, "explosion", Speculation::Speculative, Reconciliation::Cancel})
            .has_value());

    for (u64 tick = 0; tick < 8; ++tick) {
        CY_CHECK(ledger.realise(kExplosion, tick, SimulationPoint{Epoch{1}, tick}, false) ==
                 EffectVerdict::Realise);
    }
    CY_CHECK_EQ(ledger.size(), 8U);
    CY_CHECK_EQ(ledger.oldest_tick(), u64{0});

    // The rollback window advances: everything before tick 5 is beyond recall and is dropped.
    ledger.prune_before(5);
    CY_CHECK_EQ(ledger.size(), 3U);
    CY_CHECK_EQ(ledger.oldest_tick(), u64{5});

    // The capacity backstop, for a session that never prunes. It drops the oldest and COUNTS what
    // it dropped: a ledger that silently forgot would start permitting duplicates again and nothing
    // would say so.
    for (u64 tick = 8; tick < 40; ++tick) {
        CY_CHECK(ledger.realise(kExplosion, tick, SimulationPoint{Epoch{1}, tick}, false) ==
                 EffectVerdict::Realise);
    }
    CY_CHECK_LE(ledger.size(), 16U);
    CY_CHECK_GT(ledger.report().dropped_for_capacity, 0U);
}

CY_TEST_CASE("replay: an effect kind is declared once and zero is not an identity") {
    SideEffectLedger ledger(allocator());
    CY_REQUIRE(
        ledger.declare({kExplosion, "explosion", Speculation::Speculative, Reconciliation::Cancel})
            .has_value());
    CY_CHECK_FALSE(
        ledger.declare({kExplosion, "again", Speculation::Speculative, Reconciliation::Cancel})
            .has_value());
    CY_CHECK_FALSE(
        ledger.declare({0, "null", Speculation::Speculative, Reconciliation::Cancel}).has_value());
    CY_CHECK(ledger.find(kExplosion) != nullptr);
    CY_CHECK(ledger.find(kAchievement) == nullptr);
}
