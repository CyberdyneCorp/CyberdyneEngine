// M9 TASK 3.1 — PRESENTATION TRACKS THAT CANNOT AFFECT RECONSTRUCTION, AND WHY THE TOPOLOGY
// MATTERS.
//
// `replay-and-rollback` — "Presentation tracks": "Removing or ignoring a presentation track SHALL
// NOT affect authoritative reconstruction", and "A replay without a camera track still plays".
//
// ================================================================================================
// THE SECOND CASE IS THE ONE WORTH READING
// ================================================================================================
//
// It is the demonstration behind `PlaybackDriver::bind_participant()`. A replay that feeds four
// participants through ONE producer reproduces the same world — the state hash matches at every
// tick — and produces a DIFFERENT record, because `CommandBuffer::record()` stamps a per-producer
// sequence and `record_hash()` folds `command.sequence` in. That replay would then report a false
// desync the first time anyone compared its log against a peer's.
//
// So the case asserts both halves: same state, different log. It is the negative control for
// `tests/test_bitexact.cpp`'s positive one, and without it "bit-exact" would be a word rather than
// a measurement.
//
// `integration`: two four-player sessions and twenty-four ticks each.

#include "sim.h"

#include <cy/replay/playback.h>
#include <cy/replay/session.h>

using namespace cy::replay_test;
using cy::u32;
using cy::u64;
using cy::determinism::Epoch;
using cy::determinism::SimulationPoint;

namespace {

constexpr u64 kTicks = 24;

/// Record `kTicks` ticks of a four-player session into `log`, and report the final state hash.
[[nodiscard]] bool record(ToySession& session, SessionRecorder& recorder,
                          u64& final_hash) noexcept {
    recorder.attach(session.commands());
    for (u64 tick = 0; tick < kTicks; ++tick) {
        recorder.set_point(SimulationPoint{Epoch{}, tick});
        if (!session.live_tick(tick)) {
            return false;
        }
        final_hash = session.root_hash();
        if (!recorder.record_state_hash(final_hash).has_value()) {
            return false;
        }
    }
    cy::replay::SessionRecorder::detach(session.commands());
    return true;
}

}  // namespace

CY_TEST_CASE("replay: a presentation track cannot reach authoritative reconstruction") {
    RecordLog log(allocator(), manifest());
    ToySession live;
    CY_REQUIRE(live.build());
    SessionRecorder recorder(log);
    u64 recorded_hash = 0;
    CY_REQUIRE(record(live, recorder, recorded_hash));

    const u32 records_before = log.size();
    const u64 log_hash_before = log.hash();

    // Authoring a full set of tracks over the recorded session. **None of this touches the log**,
    // and that is the mechanism rather than a convention: a track is not a `LogRecord` kind, it is
    // not in the index, and `PlaybackDriver` has no way to see one.
    cy::replay::PresentationTrackSet tracks(allocator());
    auto camera = tracks.add(cy::replay::PresentationTrackKind::CameraDirection, "director");
    auto notes = tracks.add(cy::replay::PresentationTrackKind::Annotation, "commentary");
    auto markers = tracks.add(cy::replay::PresentationTrackKind::Marker, "chapters");
    CY_REQUIRE(camera.has_value());
    CY_REQUIRE(notes.has_value());
    CY_REQUIRE(markers.has_value());
    for (u64 tick = 0; tick < kTicks; tick += 4) {
        cy::replay::PresentationSample sample;
        sample.tick = tick;
        sample.subject = live.entity_of(static_cast<u32>((tick / 4) % ToySession::kPlayers)).bits();
        CY_REQUIRE((*camera)->append(sample).has_value());
        CY_REQUIRE((*notes)->append(sample).has_value());
        CY_REQUIRE((*markers)->append(sample).has_value());
    }
    CY_CHECK_EQ(log.size(), records_before);
    CY_CHECK_EQ(log.hash(), log_hash_before);

    // A replay that consults the tracks and one that does not are the same replay.
    u64 hashes[2] = {};
    for (u32 pass = 0; pass < 2; ++pass) {
        ToySession playback;
        CY_REQUIRE(playback.build());
        cy::replay::PlaybackDriver driver(allocator(), log);
        for (u32 player = 0; player < ToySession::kPlayers; ++player) {
            CY_REQUIRE(driver
                           .bind_participant(playback.participant_bits(player),
                                             playback.producer_of(player))
                           .has_value());
        }
        for (u64 tick = 0; tick < kTicks; ++tick) {
            if (pass == 0) {
                // Directed playback: the authored camera overrides the one reconstructed from
                // simulation state. Read and discarded here, because this module has no camera —
                // the point is that reading it changes nothing downstream.
                cy::replay::PresentationSample directed;
                (void)tracks.camera_override(tick, directed);
            }
            auto produced = driver.produce(playback.commands(), tick);
            CY_REQUIRE(produced.has_value());
            CY_REQUIRE(playback.step(tick));
        }
        hashes[pass] = playback.root_hash();
    }
    CY_CHECK_EQ(hashes[0], hashes[1]);
    CY_CHECK_EQ(hashes[0], recorded_hash);

    // "A replay without a camera track still plays": the set answers "no override" and the caller
    // reconstructs from simulation state, which is the default rather than a fallback.
    cy::replay::PresentationTrackSet none(allocator());
    cy::replay::PresentationSample unused;
    CY_CHECK_FALSE(none.camera_override(0, unused));
}

CY_TEST_CASE("replay: flattening the producer topology keeps the state and loses the record") {
    RecordLog log(allocator(), manifest());
    ToySession live;
    CY_REQUIRE(live.build());
    SessionRecorder recorder(log);
    u64 recorded_hash = 0;
    CY_REQUIRE(record(live, recorder, recorded_hash));

    // THE NEGATIVE CONTROL. Every participant through producer 0, which is what a playback driver
    // that did not reproduce the topology would do.
    RecordLog flattened(allocator(), manifest());
    ToySession playback;
    CY_REQUIRE(playback.build());
    SessionRecorder flat_recorder(flattened);
    cy::replay::PlaybackDriver driver(allocator(), log);
    driver.set_default_producer(playback.producer_of(0));

    flat_recorder.attach(playback.commands());
    for (u64 tick = 0; tick < kTicks; ++tick) {
        flat_recorder.set_point(SimulationPoint{Epoch{}, tick});
        auto produced = driver.produce(playback.commands(), tick);
        CY_REQUIRE(produced.has_value());
        CY_REQUIRE(playback.step(tick));
        CY_REQUIRE(flat_recorder.record_state_hash(playback.root_hash()).has_value());
    }
    cy::replay::SessionRecorder::detach(playback.commands());

    // The driver SAYS SO rather than leaving it to be discovered: every command went to a
    // participant it had no binding for.
    CY_CHECK_EQ(driver.unbound_participants(), kTicks * ToySession::kPlayers);

    // The world is right...
    CY_CHECK_EQ(playback.root_hash(), recorded_hash);
    CY_CHECK_EQ(flattened.size(), log.size());
    // ...and the record is not. This is exactly the replay that would report a false desync the
    // first time it was compared against a peer's log.
    CY_CHECK(flattened.hash() != log.hash());
}
