// M9 SECTION 3 — THE TWO CLAIMS THE MILESTONE IS JUDGED ON.
//
//   1. "A recorded replay reproduces the final state hash exactly, including after seeking."
//   2. "An injected divergence is localised to a field by the validator."
//
// `integration`: every case here builds two `ecs::World`s, four participants, a command stream and
// two hashing codecs, and runs forty ticks twice. The taxonomy's rule is about cost, and this is
// not a millisecond of arithmetic.
//
// ================================================================================================
// WHY THE CLAIM IS MADE OVER THE LOG'S HASH AS WELL AS THE STATE'S
// ================================================================================================
//
// "Reproduces the final state hash" is the criterion, and it is satisfiable by a replay that
// reached the same world through a different record — different per-producer sequence numbers, say,
// because it fed four participants through one producer. That replay is not bit-exact and the next
// thing anyone does with it is compare it against a peer's and get a false desync.
//
// So these cases assert BOTH: `RecordLog::hash()` over the replay equals the recording's, and the
// state hash equals the recording's, at every tick and not only at the end. `record_hash()`
// excludes provenance and source on purpose (a replay's commands carry `Replay` where the originals
// carried `Human`), and it includes `command.sequence`, which is precisely why `PlaybackDriver`
// reproduces the recording's producer topology instead of flattening it.
//
// ================================================================================================
// THE MUTATIONS THAT PROVE THESE ARE CHECKS
// ================================================================================================
//
//   * bind every participant to producer 0 in the replay (one line in `replay_of`) — the log hashes
//     differ while the state hashes still match, which is the exact failure the paragraph above
//     describes and the reason the log half of the assertion exists.
//   * remove the `ControlSourceKind::Replay` rewrite from `PlaybackDriver::produce()` — nothing
//     changes, and that is the point: `gameplay-framework` forbids provenance from affecting
//     anything, so this test is also the check that it does not.
//   * drop the perturbation from the divergence case — `narrow()` refuses, loudly, rather than
//     producing a report about a divergence that did not happen.

#include "sim.h"

#include <cy/core/determinism/hash.h>
#include <cy/replay/playback.h>
#include <cy/replay/session.h>
#include <cy/replay/snapshot.h>

#include <cstring>

using namespace cy::replay_test;
using cy::u32;
using cy::u64;
using cy::determinism::RunSide;
using cy::determinism::SimulationPoint;
using cy::determinism::StateHashTree;

namespace {

constexpr u64 kTicks = 40;
constexpr u64 kCheckpointEvery = 10;

/// The per-tick hashes a run produced, so a comparison can name the first tick that differed rather
/// than only the last.
struct HashTrail {
    u64 hashes[kTicks] = {};
};

}  // namespace

CY_TEST_CASE(
    "replay: a recorded session replays to the same log and the same state, tick by tick") {
    // --- The recording ---------------------------------------------------------------------------
    RecordLog recorded(allocator(), manifest());
    ToySession live;
    CY_REQUIRE(live.build());
    SessionRecorder recorder(recorded);
    cy::replay::SnapshotRing checkpoints(allocator(), 8ULL * 1024 * 1024);

    HashTrail original;
    recorder.attach(live.commands());
    for (u64 tick = 0; tick < kTicks; ++tick) {
        recorder.set_point(SimulationPoint{cy::determinism::Epoch{}, tick});
        // A CAPTURE AT (e, T) IS THE STATE BEFORE TICK T RAN — `rollback.h` writes the convention
        // down, and a capture taken after the tick would make every seek replay one tick twice.
        if (tick % kCheckpointEvery == 0) {
            CY_REQUIRE(checkpoints
                           .capture(live.world(), live.providers(),
                                    SimulationPoint{cy::determinism::Epoch{}, tick})
                           .has_value());
        }
        CY_REQUIRE(live.live_tick(tick));
        if (tick % kCheckpointEvery == 0) {
            CY_REQUIRE(recorder.record_checkpoint(tick).has_value());
        }
        original.hashes[tick] = live.root_hash();
        CY_REQUIRE(recorder.record_state_hash(original.hashes[tick]).has_value());
    }
    cy::replay::SessionRecorder::detach(live.commands());

    CY_CHECK_EQ(recorder.dropped(), 0U);
    CY_CHECK_EQ(recorder.commands_recorded(), kTicks * ToySession::kPlayers);
    // Every commanded tick, every checkpoint, every hash.
    CY_CHECK_EQ(recorded.size(), static_cast<u32>((kTicks * ToySession::kPlayers) + kTicks +
                                                  (kTicks / kCheckpointEvery)));

    // --- The replay ------------------------------------------------------------------------------
    RecordLog replayed(allocator(), manifest());
    ToySession playback;
    CY_REQUIRE(playback.build());
    SessionRecorder replay_recorder(replayed);
    cy::replay::PlaybackDriver driver(allocator(), recorded);

    // THE PRODUCER TOPOLOGY. One producer per participant, in the recording's order — see the
    // header comment for what happens without it.
    for (u32 player = 0; player < ToySession::kPlayers; ++player) {
        CY_REQUIRE(
            driver.bind_participant(playback.participant_bits(player), playback.producer_of(player))
                .has_value());
    }

    replay_recorder.attach(playback.commands());
    for (u64 tick = 0; tick < kTicks; ++tick) {
        replay_recorder.set_point(SimulationPoint{cy::determinism::Epoch{}, tick});
        auto produced = driver.produce(playback.commands(), tick);
        CY_REQUIRE(produced.has_value());
        CY_CHECK_EQ(*produced, ToySession::kPlayers);
        CY_REQUIRE(playback.step(tick));
        if (tick % kCheckpointEvery == 0) {
            CY_REQUIRE(replay_recorder.record_checkpoint(tick).has_value());
        }
        const u64 hash = playback.root_hash();
        // TICK BY TICK, not only at the end. A replay that diverged at tick 3 and converged again
        // by tick 39 would pass a final-hash-only assertion.
        CY_CHECK_EQ(hash, original.hashes[tick]);
        CY_REQUIRE(replay_recorder.record_state_hash(hash).has_value());
    }
    cy::replay::SessionRecorder::detach(playback.commands());

    CY_CHECK_EQ(driver.unbound_participants(), u64{0});
    CY_CHECK_EQ(driver.commands_produced(), kTicks * ToySession::kPlayers);

    // **THE CLAIM.** The record and the state, both.
    CY_CHECK_EQ(replayed.size(), recorded.size());
    CY_CHECK_EQ(replayed.hash(), recorded.hash());
    CY_CHECK_EQ(playback.root_hash(), original.hashes[kTicks - 1]);

    // --- Including after seeking -----------------------------------------------------------------
    //
    // Restore the checkpoint at tick 20 into a THIRD session and fast-forward to the end. The
    // checkpoint carries entity identity and provider state; the log carries the commands.
    ToySession sought;
    CY_REQUIRE(sought.build());
    cy::replay::PlaybackDriver seeker(allocator(), recorded);
    for (u32 player = 0; player < ToySession::kPlayers; ++player) {
        CY_REQUIRE(
            seeker.bind_participant(sought.participant_bits(player), sought.producer_of(player))
                .has_value());
    }

    const cy::replay::SeekPlan plan = seeker.plan_seek(20, 0);
    CY_REQUIRE(plan.valid);
    CY_REQUIRE(plan.has_checkpoint);
    CY_CHECK_EQ(plan.checkpoint_tick, u64{20});
    CY_CHECK_EQ(plan.first_tick, u64{20});
    CY_CHECK_EQ(plan.fast_forward_ticks, u64{0});
    // Fast-forward is headless by declaration, and the plan is what says so.
    CY_CHECK(plan.fast_forward_presentation == cy::replay::Presentation::Suppressed);

    cy::replay::WindowRefusal refusal = cy::replay::WindowRefusal::None;
    const cy::replay::StateCapture* capture =
        checkpoints.find(SimulationPoint{cy::determinism::Epoch{}, plan.checkpoint_tick}, refusal);
    CY_REQUIRE(capture != nullptr);
    CY_REQUIRE(capture->restore(sought.world(), sought.providers()).has_value());
    // The restore alone reproduces the state at tick 20 — before tick 20's commands ran, which is
    // the convention `rollback.h` writes down, so it equals the hash taken at the END of tick 19.
    CY_CHECK_EQ(sought.root_hash(), original.hashes[19]);

    for (u64 tick = 20; tick < kTicks; ++tick) {
        auto produced = seeker.produce(sought.commands(), tick);
        CY_REQUIRE(produced.has_value());
        CY_CHECK_EQ(*produced, ToySession::kPlayers);
        CY_REQUIRE(sought.step(tick));
        CY_CHECK_EQ(sought.root_hash(), original.hashes[tick]);
    }
    CY_CHECK_EQ(sought.root_hash(), original.hashes[kTicks - 1]);

    // A seek past everything recorded is refused rather than clamped to the end.
    const cy::replay::SeekPlan beyond = seeker.plan_seek(100'000, 20);
    CY_CHECK_FALSE(beyond.valid);
    // Backward is reported and changes nothing else: reverse playback seeks a checkpoint and
    // replays forward, because backward simulation is not attempted.
    const cy::replay::SeekPlan back = seeker.plan_seek(5, 39);
    CY_CHECK(back.valid);
    CY_CHECK(back.backward);
    CY_CHECK_EQ(back.checkpoint_tick, u64{0});
}

CY_TEST_CASE("replay: an injected divergence is narrowed to one field on one entity") {
    RecordLog log(allocator(), manifest());
    ToySession left;
    ToySession right;
    CY_REQUIRE(left.build());
    CY_REQUIRE(right.build());
    SessionRecorder recorder(log);
    recorder.attach(left.commands());

    cy::replay::DivergenceProbe probe(allocator(), log);

    constexpr u64 kInjectAt = 25;
    constexpr u32 kVictim = 2;
    StateHashTree left_tree(allocator());
    StateHashTree right_tree(allocator());

    for (u64 tick = 0; tick < kTicks; ++tick) {
        recorder.set_point(SimulationPoint{cy::determinism::Epoch{}, tick});
        CY_REQUIRE(left.live_tick(tick));
        CY_REQUIRE(right.live_tick(tick));
        if (tick % kCheckpointEvery == 0) {
            CY_REQUIRE(recorder.record_checkpoint(tick).has_value());
        }

        if (tick == kInjectAt) {
            // THE INJECTION. One field, on one entity, in one of the two runs — the shape of a real
            // desync, where two machines agree about every command and disagree about one number.
            Health damaged = right.health_of(kVictim);
            damaged.shield += 7;
            CY_REQUIRE(right.set_health(kVictim, damaged));
        }

        const u64 left_hash = left.root_hash();
        const u64 right_hash = right.root_hash();
        CY_REQUIRE(recorder.record_state_hash(left_hash).has_value());
        CY_REQUIRE(probe.observe(RunSide::Left, tick, left_hash).has_value());
        CY_REQUIRE(probe.observe(RunSide::Right, tick, right_hash).has_value());

        if (probe.diverged()) {
            // The cheap half said which tick. Pay for the expensive half HERE and nowhere else —
            // that is the whole reason the hash is hierarchical.
            CY_REQUIRE(left.hash(left_tree));
            CY_REQUIRE(right.hash(right_tree));
            break;
        }
    }
    cy::replay::SessionRecorder::detach(left.commands());

    CY_REQUIRE(probe.diverged());
    CY_CHECK_EQ(probe.first_diverging_tick(), kInjectAt);
    CY_CHECK_EQ(probe.last_agreeing_tick(), kInjectAt - 1);

    cy::replay::DivergenceReport report;
    CY_REQUIRE(probe.narrow(left_tree, right_tree, report).has_value());
    CY_REQUIRE(report.valid);
    CY_REQUIRE(report.field.diverged);
    CY_CHECK_FALSE(report.field.shape_mismatch);

    // **NARROWED, NOT MERELY DETECTED.** The entity, the component and the field, each by identity
    // and by name.
    CY_CHECK_EQ(report.field.entity, right.entity_of(kVictim).bits());
    CY_CHECK_EQ(report.field.component, static_cast<u64>(kHealthSubject.value));
    CY_CHECK(std::strcmp(report.field.component_name, "Health") == 0);
    CY_CHECK_EQ(report.field.field, u64{2});
    CY_CHECK(std::strcmp(report.field.field_name, "shield") == 0);
    CY_CHECK(report.field.left != report.field.right);
    // It is NOT the other component, NOT another entity, and NOT the other field of the same
    // component — a localiser that named the first thing it found would pass the three lines above.
    CY_CHECK(report.field.component != static_cast<u64>(kPositionSubject.value));
    for (u32 player = 0; player < ToySession::kPlayers; ++player) {
        if (player != kVictim) {
            CY_CHECK(report.field.entity != right.entity_of(player).bits());
        }
    }

    // The window: the last agreeing checkpoint, the commands in between, the seed. The random trace
    // is derivable from the seed and is deliberately not recorded — `divergence.h` says why.
    CY_CHECK(report.window.valid);
    CY_CHECK(report.window.has_last_agreeing);
    CY_CHECK_EQ(report.window.first_diverging_tick, kInjectAt);
    CY_CHECK(report.window.has_checkpoint);
    CY_CHECK_EQ(report.window.checkpoint.tick, u64{20});
    CY_CHECK_EQ(report.window.command_count, ToySession::kPlayers);  // tick 25's four commands
    CY_CHECK_EQ(report.window.session_seed, manifest().session_seed);
    CY_CHECK(report.window.left_hash != report.window.right_hash);

    // The line a person reads. Its shape IS the claim, so the case reads it rather than trusting
    // it.
    char line[cy::replay::kDivergenceReportBuffer] = {};
    const cy::usize written = cy::replay::format_divergence(line, sizeof(line), report);
    CY_REQUIRE(written > 0);
    CY_CHECK(std::strstr(line, "Health") != nullptr);
    CY_CHECK(std::strstr(line, "shield") != nullptr);
    CY_CHECK(std::strstr(line, "tick 25") != nullptr);
    CY_TEST_MESSAGE(line);
}

CY_TEST_CASE("replay: narrowing refuses a comparison that did not diverge") {
    RecordLog log(allocator(), manifest());
    ToySession left;
    ToySession right;
    CY_REQUIRE(left.build());
    CY_REQUIRE(right.build());

    cy::replay::DivergenceProbe probe(allocator(), log);
    for (u64 tick = 0; tick < 12; ++tick) {
        CY_REQUIRE(left.live_tick(tick));
        CY_REQUIRE(right.live_tick(tick));
        const u64 hash = left.root_hash();
        CY_CHECK_EQ(hash, right.root_hash());
        CY_REQUIRE(probe.observe(RunSide::Left, tick, hash).has_value());
        CY_REQUIRE(probe.observe(RunSide::Right, tick, right.root_hash()).has_value());
    }
    CY_CHECK_FALSE(probe.diverged());
    CY_CHECK_EQ(probe.ticks_compared(), 12U);

    StateHashTree left_tree(allocator());
    StateHashTree right_tree(allocator());
    CY_REQUIRE(left.hash(left_tree));
    CY_REQUIRE(right.hash(right_tree));
    cy::replay::DivergenceReport report;
    // THE REFUSAL. A "report" about a divergence that did not happen is the shape of a check that
    // cannot fail, and this project has shipped three of those.
    CY_CHECK_FALSE(probe.narrow(left_tree, right_tree, report).has_value());
    CY_CHECK_FALSE(report.valid);

    char line[cy::replay::kDivergenceReportBuffer] = {};
    CY_REQUIRE(cy::replay::format_divergence(line, sizeof(line), report) > 0);
    CY_CHECK(std::strcmp(line, "divergence: none") == 0);
}
