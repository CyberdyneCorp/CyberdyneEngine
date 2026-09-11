// M9 TASK 3.4 — THE CRASH REPLAY BUFFER, BOUNDED, AND FLUSHED INTO SOMETHING LOADABLE.
//
// `replay-and-rollback`: the buffer "SHALL be attachable to the report together with build
// identity, simulation point, recent state hashes, and the divergence capture if one exists", and
// "The resulting artefact SHALL be **loadable to reproduce the final seconds of the session**."
//
// ================================================================================================
// THE LAST SENTENCE IS THE ONLY ONE WORTH TESTING, AND THIS FILE TESTS IT BY RE-SIMULATING
// ================================================================================================
//
// "Attachable" is satisfied by any pile of bytes and proves nothing. So the second case here takes
// the artefact's bytes, reads them back into a *different* object, turns them into a `RecordLog`,
// and drives a fresh four-player session through the recovered window — then compares the resulting
// state hash against the hash the artefact itself carries. An artefact whose records decoded and
// whose replay produced a different world would pass every structural check and be worthless.
//
// ================================================================================================
// THE GAP, STATED HERE RATHER THAN DISCOVERED AT THE GATE
// ================================================================================================
//
// The world the window starts from comes from the session's **checkpoint store**, not from the
// artefact's bytes. `src/replay/README.md` §4 records why and section 1 found it first: a
// restorable checkpoint holds an `ecs::Snapshot`, and `src/ecs/` has no identity-preserving
// serialiser for one — `serialize()` writes values and `deserialize()` mints fresh entities, which
// would put the right values under the wrong identities and report a divergence on every entity
// restored.
//
// So what this artefact reproduces off-machine today is the window's INPUTS — every command, every
// external result, every state hash — and not its starting world. That is enough for
// `replay-and-rollback`'s own scenario ("a tester reports that units stopped moving") when the
// developer has the session's checkpoints, and it is not enough to post a crash artefact to a
// stranger. Closing it is a change to `src/ecs/`, which is not this phase's to make.
//
// `integration`: forty ticks of a four-player session, twice.

#include "sim.h"

#include <cy/replay/crash.h>
#include <cy/replay/playback.h>
#include <cy/replay/session.h>
#include <cy/replay/snapshot.h>

#include <cstring>

using namespace cy::replay_test;
using cy::u32;
using cy::u64;
using cy::determinism::Epoch;
using cy::determinism::SimulationPoint;

namespace {

constexpr u64 kTicks = 40;
constexpr u64 kCheckpointEvery = 5;

}  // namespace

CY_TEST_CASE("replay: the crash ring is bounded and the artefact says what it lost") {
    RecordLog log(allocator(), manifest());
    ToySession session;
    CY_REQUIRE(session.build());
    SessionRecorder recorder(log);

    // A one-second window at 60 Hz with one record a tick's worth of room — deliberately far too
    // small for forty ticks of four players, so the ring genuinely wraps and the artefact has to be
    // honest about it.
    const u32 capacity = cy::replay::crash_ring_records(1, 60, 1, 1);
    CY_REQUIRE_EQ(capacity, 60U);
    cy::replay::CrashReplayBuffer ring(allocator());
    CY_REQUIRE(ring.reserve(capacity).has_value());
    recorder.mirror_to(&ring);
    recorder.attach(session.commands());

    u64 final_hash = 0;
    for (u64 tick = 0; tick < kTicks; ++tick) {
        recorder.set_point(SimulationPoint{Epoch{}, tick});
        CY_REQUIRE(session.live_tick(tick));
        if (tick % kCheckpointEvery == 0) {
            CY_REQUIRE(recorder.record_checkpoint(tick).has_value());
        }
        final_hash = session.root_hash();
        CY_REQUIRE(recorder.record_state_hash(final_hash).has_value());
    }
    cy::replay::SessionRecorder::detach(session.commands());

    // The ring saw everything the log saw — one record, two destinations, and no path by which they
    // can disagree.
    CY_CHECK_EQ(ring.pushed(), static_cast<u64>(log.size()));
    CY_CHECK_EQ(ring.size(), capacity);
    CY_CHECK(ring.overwritten() > 0);

    cy::replay::CrashArtefact artefact(allocator());
    CY_REQUIRE(artefact
                   .assemble(manifest(), ring, cy::replay::CrashTrigger::ReportedDefect,
                             SimulationPoint{Epoch{}, kTicks - 1})
                   .has_value());
    CY_CHECK_EQ(artefact.records().size(), static_cast<cy::usize>(capacity));
    CY_CHECK_EQ(artefact.last_tick(), kTicks - 1);
    CY_CHECK(artefact.first_tick() > 0);  // the ring does not reach the start of the session
    CY_CHECK_EQ(artefact.records_lost(), ring.overwritten());
    CY_CHECK(artefact.trigger() == cy::replay::CrashTrigger::ReportedDefect);

    // The recent hashes are DERIVED from the records rather than passed in beside them, so they
    // cannot disagree with the records in the same artefact.
    CY_REQUIRE(artefact.recent_hash_count() > 0);
    CY_CHECK_EQ(artefact.recent_hash(artefact.recent_hash_count() - 1), final_hash);
    CY_CHECK_EQ(artefact.recent_hash_tick(artefact.recent_hash_count() - 1), kTicks - 1);

    // An empty ring reproduces nothing, and an artefact that reproduces nothing is worse than none:
    // somebody will spend an afternoon loading it.
    cy::replay::CrashReplayBuffer empty(allocator());
    CY_REQUIRE(empty.reserve(8).has_value());
    cy::replay::CrashArtefact nothing(allocator());
    CY_CHECK_FALSE(nothing
                       .assemble(manifest(), empty, cy::replay::CrashTrigger::Crash,
                                 SimulationPoint{Epoch{}, 0})
                       .has_value());
}

CY_TEST_CASE("replay: a crash artefact round-trips, and its window re-simulates to its own hash") {
    RecordLog log(allocator(), manifest());
    ToySession session;
    CY_REQUIRE(session.build());
    SessionRecorder recorder(log);
    cy::replay::SnapshotRing checkpoints(allocator(), 8ULL * 1024 * 1024);

    cy::replay::CrashReplayBuffer ring(allocator());
    CY_REQUIRE(ring.reserve(60).has_value());
    recorder.mirror_to(&ring);
    recorder.attach(session.commands());

    u64 final_hash = 0;
    for (u64 tick = 0; tick < kTicks; ++tick) {
        recorder.set_point(SimulationPoint{Epoch{}, tick});
        if (tick % kCheckpointEvery == 0) {
            CY_REQUIRE(
                checkpoints
                    .capture(session.world(), session.providers(), SimulationPoint{Epoch{}, tick})
                    .has_value());
        }
        CY_REQUIRE(session.live_tick(tick));
        if (tick % kCheckpointEvery == 0) {
            CY_REQUIRE(recorder.record_checkpoint(tick).has_value());
        }
        final_hash = session.root_hash();
        CY_REQUIRE(recorder.record_state_hash(final_hash).has_value());
    }
    cy::replay::SessionRecorder::detach(session.commands());

    cy::replay::CrashArtefact artefact(allocator());
    CY_REQUIRE(artefact
                   .assemble(manifest(), ring, cy::replay::CrashTrigger::Crash,
                             SimulationPoint{Epoch{}, kTicks - 1})
                   .has_value());

    // "and the divergence capture if one exists". Carried, names included — `FieldDivergence` holds
    // pointers into a schema that will not survive the file.
    cy::replay::DivergenceReport injected;
    injected.valid = true;
    injected.field.diverged = true;
    injected.field.entity = 0x1234;
    injected.field.component = kHealthSubject.value;
    injected.field.component_name = "Health";
    injected.field.field = 2;
    injected.field.field_name = "shield";
    injected.field.left = 0xAAAA;
    injected.field.right = 0xBBBB;
    injected.field.depth = 4;
    injected.window.valid = true;
    injected.window.first_diverging_tick = 37;
    injected.window.last_agreeing_tick = 36;
    injected.window.session_seed = manifest().session_seed;
    injected.window.command_count = 4;
    CY_REQUIRE(artefact.attach_divergence(injected).has_value());
    // A report that was never narrowed is refused rather than carried as an empty one.
    CY_CHECK_FALSE(artefact.attach_divergence(cy::replay::DivergenceReport{}).has_value());

    cy::Array<cy::u8> bytes(allocator());
    CY_REQUIRE(artefact.write(bytes).has_value());
    // Deterministic: the same artefact written twice produces the same bytes, which is what lets a
    // bug tracker deduplicate reports by digest.
    cy::Array<cy::u8> again(allocator());
    CY_REQUIRE(artefact.write(again).has_value());
    CY_REQUIRE_EQ(bytes.size(), again.size());
    CY_CHECK(std::memcmp(bytes.data(), again.data(), bytes.size()) == 0);

    // --- Read it back, into a different object
    // ------------------------------------------------------
    cy::replay::CrashArtefact loaded(allocator());
    cy::replay::RejectReason why = cy::replay::RejectReason::None;
    CY_REQUIRE(loaded.read(bytes.span(), why).has_value());
    CY_CHECK(why == cy::replay::RejectReason::None);
    CY_CHECK(loaded.trigger() == cy::replay::CrashTrigger::Crash);
    CY_CHECK_EQ(loaded.point().tick, kTicks - 1);
    CY_CHECK_EQ(loaded.manifest().session_seed, manifest().session_seed);
    CY_CHECK_EQ(loaded.manifest().engine_build, manifest().engine_build);
    CY_REQUIRE_EQ(loaded.records().size(), artefact.records().size());
    for (cy::usize index = 0; index < loaded.records().size(); ++index) {
        CY_CHECK_EQ(cy::replay::record_hash(0, loaded.records()[index]),
                    cy::replay::record_hash(0, artefact.records()[index]));
    }
    CY_REQUIRE(loaded.has_divergence());
    CY_CHECK_EQ(loaded.divergence().field.entity, u64{0x1234});
    CY_CHECK(std::strcmp(loaded.divergence().field.component_name, "Health") == 0);
    CY_CHECK(std::strcmp(loaded.divergence().field.field_name, "shield") == 0);
    CY_CHECK_EQ(loaded.divergence().window.first_diverging_tick, u64{37});

    // Damage is distinguished from incompatibility, and neither is inferred from the other.
    cy::replay::CrashArtefact rubbish(allocator());
    cy::Array<cy::u8> noise(allocator());
    for (u32 index = 0; index < 64; ++index) {
        CY_REQUIRE(noise.push_back(static_cast<cy::u8>(index)).has_value());
    }
    CY_CHECK_FALSE(rubbish.read(noise.span(), why).has_value());
    CY_CHECK(why == cy::replay::RejectReason::Corrupt);
    // Truncated after a valid header: still `Corrupt`, and still a refusal rather than a partial
    // read of whatever followed the buffer in memory.
    cy::replay::CrashArtefact truncated(allocator());
    CY_CHECK_FALSE(
        truncated.read(cy::Span<const cy::u8>{bytes.data(), bytes.size() / 2}, why).has_value());
    CY_CHECK(why == cy::replay::RejectReason::Corrupt);

    // --- **THE CLAIM**: loadable to reproduce the final seconds
    // -----------------------------------
    RecordLog recovered(allocator(), loaded.manifest());
    CY_REQUIRE(loaded.to_log(recovered).has_value());
    CY_CHECK_EQ(recovered.size(), static_cast<u32>(loaded.records().size()));

    LogRecord checkpoint;
    CY_REQUIRE(recovered.nearest_checkpoint(loaded.last_tick(), checkpoint));
    // The checkpoint the window starts from must be inside the window and not at its ragged edge,
    // or the first tick would be re-simulated with only the commands the ring happened to keep.
    CY_REQUIRE(checkpoint.tick > loaded.first_tick());

    ToySession reproduction;
    CY_REQUIRE(reproduction.build());
    cy::replay::WindowRefusal refusal = cy::replay::WindowRefusal::None;
    const cy::replay::StateCapture* capture =
        checkpoints.find(SimulationPoint{Epoch{}, checkpoint.tick}, refusal);
    CY_REQUIRE(capture != nullptr);
    CY_REQUIRE(capture->restore(reproduction.world(), reproduction.providers()).has_value());

    cy::replay::PlaybackDriver driver(allocator(), recovered);
    for (u32 player = 0; player < ToySession::kPlayers; ++player) {
        CY_REQUIRE(driver
                       .bind_participant(reproduction.participant_bits(player),
                                         reproduction.producer_of(player))
                       .has_value());
    }
    const cy::replay::SeekPlan plan = driver.plan_seek(checkpoint.tick, 0);
    CY_REQUIRE(plan.valid);
    CY_CHECK_EQ(plan.first_tick, checkpoint.tick);

    for (u64 tick = checkpoint.tick; tick <= loaded.last_tick(); ++tick) {
        auto produced = driver.produce(reproduction.commands(), tick);
        CY_REQUIRE(produced.has_value());
        CY_CHECK_EQ(*produced, ToySession::kPlayers);
        CY_REQUIRE(reproduction.step(tick));
    }

    // The window re-simulated to the hash the artefact itself carries. Not "the file parsed" — the
    // final seconds of the session actually happened again.
    CY_CHECK_EQ(reproduction.root_hash(), final_hash);
    CY_CHECK_EQ(reproduction.root_hash(), loaded.recent_hash(loaded.recent_hash_count() - 1));
    CY_CHECK_EQ(driver.unbound_participants(), u64{0});
}
