// M9 TASK 3.2 — THE DUPLICATE-EFFECT PROOF, AND ROLLBACK REACHING THE HASH IT ROLLED BACK FROM.
//
// `docs/ROADMAP.md`'s exit criterion: "Rollback re-simulates without re-applying ledgered side
// effects, **proven by a duplicate-effect test**." `replay-and-rollback`'s M9 delta adds: "The test
// that proves it SHALL fail when the ledger is removed, and that SHALL be **demonstrated** rather
// than asserted."
//
// ================================================================================================
// THE MUTATION, AND WHY IT IS IN `RollbackEngine::offer()` RATHER THAN IN THE LEDGER
// ================================================================================================
//
// `src/replay/tests/test_ledger.cpp` already mutates `SideEffectLedger::realise()` and shows the
// ledger's own arithmetic going red. That proves the ledger suppresses a duplicate key; it does not
// prove that the ROLLBACK LOOP consults it, which is what the exit criterion is about — a loop that
// re-simulated without ever calling `offer()` would pass every case in that file.
//
// So the mutation here is one line in `RollbackEngine::offer()`:
//
//     const EffectVerdict verdict = ledger_->realise(kind, instance, at_, predicted);
//  -> const EffectVerdict verdict = EffectVerdict::Realise;
//
// The window, the restore, the cursor and the re-simulation all survive it; only the ledger's
// decision is gone, and `explosions_played` climbs. The run is pasted in `src/replay/README.md`.
//
// `integration`: an `ecs::World`, four participants, a snapshot ring and twenty-five ticks twice.

#include "sim.h"

#include <cy/replay/ledger.h>
#include <cy/replay/rollback.h>
#include <cy/replay/session.h>

using namespace cy::replay_test;
using cy::u32;
using cy::u64;
using cy::determinism::SimulationPoint;

namespace {

constexpr u64 kLiveTicks = 25;
constexpr u64 kRollBackTo = 12;

/// The session's presentation side. `played` counts the explosions a player would have SEEN.
struct Explosions {
    cy::replay::RollbackEngine* engine = nullptr;
    u32 played = 0;
    u32 suppressed = 0;
    u32 deferred = 0;

    static void offer(void* user, u64 kind, u64 instance) noexcept {
        auto* self = static_cast<Explosions*>(user);
        switch (self->engine->offer(kind, instance, /*predicted=*/false)) {
            case cy::replay::EffectVerdict::Realise:
                ++self->played;
                break;
            case cy::replay::EffectVerdict::SuppressedAlreadyRealised:
                ++self->suppressed;
                break;
            case cy::replay::EffectVerdict::DeferredUntilConfirmed:
                ++self->deferred;
                break;
            case cy::replay::EffectVerdict::Undeclared:
                break;
        }
    }
};

/// The two hooks the rollback loop borrows from the session. Deliberately the SAME `step` the live
/// loop calls — `RollbackHooks` is handed no flag saying which it is.
struct Hooks {
    ToySession* session = nullptr;
    u64 last_tick = 0;

    static cy::Status feed(void* user, u64 tick,
                           cy::Span<const cy::replay::LogRecord> commands) noexcept {
        auto* self = static_cast<Hooks*>(user);
        (void)tick;
        return self->session->feed(commands)
                   ? cy::ok()
                   : cy::fail(cy::ErrorCode::Unknown, "the fixture refused a recorded command");
    }

    static cy::Status step(void* user, u64 tick) noexcept {
        auto* self = static_cast<Hooks*>(user);
        self->last_tick = tick;
        return self->session->step(tick)
                   ? cy::ok()
                   : cy::fail(cy::ErrorCode::Unknown, "the fixture refused to step");
    }

    [[nodiscard]] cy::replay::RollbackHooks hooks() noexcept {
        return cy::replay::RollbackHooks{&Hooks::feed, &Hooks::step, this};
    }
};

}  // namespace

CY_TEST_CASE("replay: re-simulating after a rollback does not play the explosion twice") {
    RecordLog log(allocator(), manifest());
    ToySession session;
    CY_REQUIRE(session.build());
    SessionRecorder recorder(log);
    recorder.attach(session.commands());

    cy::replay::SnapshotRing ring(allocator(), 16ULL * 1024 * 1024);
    cy::replay::SideEffectLedger ledger(allocator());
    CY_REQUIRE(ledger
                   .declare(cy::replay::EffectDeclaration{kExplosionKind, "explosion",
                                                          cy::replay::Speculation::Speculative,
                                                          cy::replay::Reconciliation::Cancel})
                   .has_value());
    cy::determinism::EpochCounter epochs;
    cy::replay::RollbackEngine engine(allocator(), log, ring, ledger, epochs);

    Explosions explosions;
    explosions.engine = &engine;
    session.set_effect_sink(&Explosions::offer, &explosions);

    // --- The live run
    // -----------------------------------------------------------------------------
    u64 hashes[kLiveTicks] = {};
    for (u64 tick = 0; tick < kLiveTicks; ++tick) {
        // A rollback capture per tick, which is what a rollback window is: "an in-memory ring of
        // rollback snapshots over a bounded window". Taken BEFORE the tick runs — `rollback.h`
        // writes the convention down.
        CY_REQUIRE(ring.capture(session.world(), session.providers(),
                                SimulationPoint{epochs.current(), tick})
                       .has_value());
        recorder.set_point(SimulationPoint{epochs.current(), tick});
        engine.set_point(SimulationPoint{epochs.current(), tick});
        CY_REQUIRE(session.live_tick(tick));
        hashes[tick] = session.root_hash();
        CY_REQUIRE(recorder.record_state_hash(hashes[tick]).has_value());
    }
    cy::replay::SessionRecorder::detach(session.commands());

    const u32 played_live = explosions.played;
    CY_REQUIRE(played_live > 0);  // a session that offered nothing would prove nothing
    CY_CHECK_EQ(explosions.suppressed, 0U);
    CY_CHECK_EQ(engine.effects_offered(), static_cast<u64>(session.effects_offered()));

    // --- The rollback
    // -----------------------------------------------------------------------------
    Hooks hooks;
    hooks.session = &session;
    cy::replay::RollbackReport report;
    CY_REQUIRE(engine
                   .roll_back(kRollBackTo, kLiveTicks - 1, session.world(), session.providers(),
                              hooks.hooks(), report)
                   .has_value());
    CY_REQUIRE(report.performed);
    CY_CHECK(report.refusal == cy::replay::WindowRefusal::None);
    CY_CHECK_EQ(report.restored_tick, kRollBackTo);
    CY_CHECK_EQ(report.ticks_resimulated, static_cast<u32>(kLiveTicks - kRollBackTo));
    CY_CHECK_EQ(report.commands_replayed,
                static_cast<u32>((kLiveTicks - kRollBackTo) * ToySession::kPlayers));
    // The timeline was reset, so the epoch was left. `epoch.h` requires the reason.
    CY_CHECK_EQ(epochs.current().value, 1U);
    CY_CHECK(epochs.reason() == cy::determinism::EpochReason::CheckpointRestore);
    CY_CHECK_EQ(report.epoch.value, 1U);

    // **THE CLAIM.** Every explosion in the re-simulated window was offered again and none of them
    // played again.
    CY_CHECK_EQ(explosions.played, played_live);
    CY_CHECK(report.effects_offered > 0);
    CY_CHECK_EQ(report.effects_suppressed, report.effects_offered);
    CY_CHECK_EQ(explosions.suppressed, report.effects_suppressed);

    // And the re-simulation reached the state it rolled back from — which is the other half,
    // because a loop that suppressed every effect by never running would also report zero
    // duplicates.
    CY_CHECK_EQ(session.root_hash(), hashes[kLiveTicks - 1]);

    // --- The control, in the other direction
    // --------------------------------------------------------
    //
    // A ledger that suppressed EVERYTHING would pass every assertion above and is the same defect
    // as one that suppresses nothing. Ticks the ledger has never seen still play — run forward
    // until the simulation next offers one, because whether an effect fires is a function of state
    // and "the very next tick" is not guaranteed to produce one.
    const u32 before_new_ticks = explosions.played;
    const u32 offered_before = session.effects_offered();
    for (u64 tick = kLiveTicks; tick < kLiveTicks + 12; ++tick) {
        engine.set_point(SimulationPoint{epochs.current(), tick});
        CY_REQUIRE(session.live_tick(tick));
    }
    CY_REQUIRE(session.effects_offered() > offered_before);  // or the control tests nothing
    CY_CHECK(explosions.played > before_new_ticks);
}

CY_TEST_CASE("replay: a rollback older than the window is reported, not attempted") {
    RecordLog log(allocator(), manifest());
    ToySession session;
    CY_REQUIRE(session.build());
    SessionRecorder recorder(log);
    recorder.attach(session.commands());

    // A budget of one byte. `SnapshotRing` never evicts its newest capture — "a window of one is
    // short, and no window at all is a different failure" — so this is the shortest real window
    // there is, and tick 0 is certainly outside it.
    cy::replay::SnapshotRing ring(allocator(), 1);
    cy::replay::SideEffectLedger ledger(allocator());
    cy::determinism::EpochCounter epochs;
    cy::replay::RollbackEngine engine(allocator(), log, ring, ledger, epochs);

    for (u64 tick = 0; tick < 12; ++tick) {
        CY_REQUIRE(ring.capture(session.world(), session.providers(),
                                SimulationPoint{epochs.current(), tick})
                       .has_value());
        recorder.set_point(SimulationPoint{epochs.current(), tick});
        CY_REQUIRE(session.live_tick(tick));
    }
    cy::replay::SessionRecorder::detach(session.commands());
    CY_REQUIRE(ring.evictions() > 0);  // the budget bit, or this case tests nothing
    CY_CHECK_EQ(ring.size(), 1U);

    Hooks hooks;
    hooks.session = &session;
    cy::replay::RollbackReport report;

    // A REPORT AND NOT AN ERROR. "a request older than the window SHALL be reported as requiring
    // full resynchronisation rather than triggering an unbounded replay."
    CY_REQUIRE(engine.roll_back(0, 11, session.world(), session.providers(), hooks.hooks(), report)
                   .has_value());
    CY_CHECK_FALSE(report.performed);
    CY_CHECK(report.refusal == cy::replay::WindowRefusal::RequiresResynchronisation);
    CY_CHECK_EQ(report.ticks_resimulated, 0U);
    CY_CHECK_EQ(engine.refusals(), 1U);
    CY_CHECK_EQ(epochs.current().value, 0U);  // nothing happened, so no epoch was left

    // A backwards window and a missing step hook are failures of the REQUEST rather than of the
    // window, so they are `Status` failures and not refusals — a caller that had to tell them apart
    // by parsing a message would not.
    CY_CHECK_FALSE(
        engine.roll_back(9, 4, session.world(), session.providers(), hooks.hooks(), report)
            .has_value());
    CY_CHECK_FALSE(engine
                       .roll_back(9, 11, session.world(), session.providers(),
                                  cy::replay::RollbackHooks{}, report)
                       .has_value());
}

CY_TEST_CASE("replay: the window's advance prunes the ledger it no longer speaks for") {
    RecordLog log(allocator(), manifest());
    cy::replay::SnapshotRing ring(allocator(), 1024ULL * 1024);
    cy::replay::SideEffectLedger ledger(allocator());
    CY_REQUIRE(ledger
                   .declare(cy::replay::EffectDeclaration{kExplosionKind, "explosion",
                                                          cy::replay::Speculation::Speculative,
                                                          cy::replay::Reconciliation::Cancel})
                   .has_value());
    cy::determinism::EpochCounter epochs;
    cy::replay::RollbackEngine engine(allocator(), log, ring, ledger, epochs);

    for (u64 tick = 0; tick < 20; ++tick) {
        engine.set_point(SimulationPoint{epochs.current(), tick});
        CY_CHECK(engine.offer(kExplosionKind, 0xABCD, false) == cy::replay::EffectVerdict::Realise);
    }
    CY_CHECK_EQ(ledger.size(), 20U);

    engine.advance_window(15);
    CY_CHECK_EQ(ledger.size(), 5U);
    CY_CHECK_EQ(ledger.oldest_tick(), u64{15});

    // A tick the ledger has forgotten is realised again, and that is the honest behaviour: the
    // ledger is bounded, and a bounded ledger cannot suppress what it no longer holds. The
    // session's rollback window is bounded by the same advance, so there is nothing to roll back
    // that far.
    engine.set_point(SimulationPoint{epochs.current(), u64{3}});
    CY_CHECK(engine.offer(kExplosionKind, 0xABCD, false) == cy::replay::EffectVerdict::Realise);
}

CY_TEST_CASE("replay: an undeclared effect is refused and a confirmed-only effect waits") {
    RecordLog log(allocator(), manifest());
    cy::replay::SnapshotRing ring(allocator(), 1024ULL * 1024);
    cy::replay::SideEffectLedger ledger(allocator());
    cy::determinism::EpochCounter epochs;
    cy::replay::RollbackEngine engine(allocator(), log, ring, ledger, epochs);
    engine.set_point(SimulationPoint{epochs.current(), u64{4}});

    // Undeclared: refused rather than assumed speculative, because an undeclared achievement would
    // otherwise fire on a prediction.
    CY_CHECK(engine.offer(0xDEAD, 1, false) == cy::replay::EffectVerdict::Undeclared);

    constexpr u64 kAchievement = 0xAC41'E7EDULL;
    CY_REQUIRE(ledger
                   .declare(cy::replay::EffectDeclaration{kAchievement, "achievement",
                                                          cy::replay::Speculation::ConfirmedOnly,
                                                          cy::replay::Reconciliation::Cancel})
                   .has_value());
    CY_CHECK(engine.offer(kAchievement, 1, /*predicted=*/true) ==
             cy::replay::EffectVerdict::DeferredUntilConfirmed);
    CY_CHECK_EQ(engine.effects_deferred(), u64{1});
    CY_CHECK_EQ(ledger.confirm(4), 1U);
}
