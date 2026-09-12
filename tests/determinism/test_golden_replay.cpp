// GOLDEN REPLAYS: a recorded session with committed hashes, replayed. M10 task 6.4.
//
// `testing-and-quality` names this suite's first job in one sentence — "golden replays: recorded
// sessions with committed hashes, replayed in CI, so a regression and a deliberate behaviour change
// are distinguishable" — and `tests/determinism/README.md` has listed it as owed since M0. It did
// not exist anywhere in the tree before this file.
//
// ================================================================================================
// THREE CASES, AND EACH ONE FAILS FOR A DIFFERENT REASON
// ================================================================================================
//
//   the bytes      A session recorded today encodes to the committed file, byte for byte. This is
//                  the half that catches a change in what RECORDING produces: a record that grew a
//                  field, a chunk that compresses differently, a producer topology that changed.
//   the hashes     The COMMITTED bytes are read back and replayed into a fresh session, and the
//                  state hash after every tick equals the committed one. This is the half that
//                  catches a change in what SIMULATING produces, and it is the half
//                  `src/replay/tests/test_bitexact.cpp` structurally cannot make: that suite
//                  records and replays in one process, so a simulation that started doing something
//                  else changes both sides of its comparison at once and it stays green.
//   the refusal    A golden file from a build this one cannot claim to reproduce is REFUSED with a
//                  reason, rather than replayed to a mismatch that reads like a regression. That is
//                  what makes the two cases above distinguishable from a stale artefact.
//
// ================================================================================================
// HOW A DELIBERATE BEHAVIOUR CHANGE IS MADE
// ================================================================================================
//
//   CY_DETERMINISM_RECORD_GOLDEN=1 ctest --test-dir <build> -R determinism.golden_replay
//
// rewrites both files and FAILS the run, so a regeneration cannot happen by accident inside a green
// CI job. The diff is then reviewed: `toy-session-v1.hashes` carries one hash per tick, so it names
// the first tick at which the simulation moved rather than reporting that a number changed.
//
// ================================================================================================
// THE MUTATIONS THAT PROVE THESE ARE CHECKS
// ================================================================================================
//
// All five were APPLIED to this tree and the suite was WATCHED; the numbers are what came back.
//
//   * change what the SIMULATION does — `ToySession::step()`'s health system fires on `tick % 8`
//     rather than `tick % 7` — and BOTH cases go red: "the bytes" at 27,309 against the committed
//     27,308, and "the hashes" at seven ticks, each naming the pair that differed.
//   * change what the SCRIPT does — `ToySession::script()`'s `MoveIntent{2, …}` — and ONLY "the
//     bytes" goes red. That is correct and worth knowing: a replay's commands come out of the log,
//     so a change to what a live session would have INTENDED is invisible to a replay of a session
//     recorded before it. The two cases are not redundant; each is blind where the other sees.
//   * flip one bit of one tick's committed hash and "the hashes" fails at that tick and nowhere
//     else — which is the whole reason the expectation is a hash per tick rather than one number.
//   * remove the `out.hash() != recorded_hash` branch from `read_log()` and nothing here moves:
//     five passed, one failed, and the one was test_replay_fuzz.cpp. The split is deliberate —
//     this suite is about what the engine COMPUTES, that one about what it READS.
//   * make `classify()` return `{Reproducible, None}` unconditionally and "the refusal" goes red on
//     both halves — the verdict and the reason — which is the case that keeps a stale artefact from
//     being reported as a simulation regression.

#include "golden_session.h"

#include <cy/replay/playback.h>
#include <cy/test/fixtures.h>

#include <string>

using namespace cy::determinism_test;
using cy::u64;

namespace {

/// The committed log, as bytes. Empty when the file is missing, which every case reports as a
/// failure rather than as an empty comparison.
[[nodiscard]] std::string read_golden(const char* name) {
    std::string contents;
    if (!cy::test::read_file(golden_path(name), contents)) {
        return {};
    }
    return contents;
}

[[nodiscard]] std::string as_text(const cy::Array<cy::u8>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

}  // namespace

CY_TEST_CASE("golden: a session recorded today encodes to the committed bytes") {
    Recording fresh(cy::replay_test::allocator());
    CY_REQUIRE(record_golden_session(fresh));
    CY_CHECK_EQ(fresh.hashes.size(), static_cast<cy::usize>(kGoldenTicks));

    if (recording_requested()) {
        // REGENERATION FAILS THE RUN ON PURPOSE. A mode that rewrote the expectation and reported
        // success is a mode that turns every future regression green the first time somebody sets
        // the variable in a CI job.
        CY_CHECK(cy::test::write_file(golden_path(kGoldenLogName), as_text(fresh.bytes)));
        CY_CHECK(cy::test::write_file(golden_path(kGoldenHashesName), format_hashes(fresh)));
        CY_TEST_FAIL(
            "golden artefacts rewritten: review the diff and re-run without "
            "CY_DETERMINISM_RECORD_GOLDEN");
        return;
    }

    const std::string committed = read_golden(kGoldenLogName);
    CY_REQUIRE(!committed.empty());
    // Byte for byte. `write_log()` is documented deterministic — the same log produces the same
    // bytes — so anything but equality here is a change in the record, the chunking or the
    // compressor, and the file name in the message says which artefact to look at.
    CY_CHECK_EQ(committed.size(), fresh.bytes.size());
    CY_CHECK(committed == as_text(fresh.bytes));
}

CY_TEST_CASE("golden: the committed replay reproduces the committed state hash at every tick") {
    const std::string committed = read_golden(kGoldenLogName);
    const std::string expected_text = read_golden(kGoldenHashesName);
    CY_REQUIRE(!committed.empty());
    CY_REQUIRE(!expected_text.empty());

    const GoldenHashes expected = parse_hashes(expected_text);
    // A hashes file that parsed to nothing would make every comparison below vacuous, which is the
    // shape of failure this project has shipped before.
    CY_REQUIRE(expected.hashes.size() == static_cast<cy::usize>(kGoldenTicks));

    cy::replay::RecordLog golden(cy::replay_test::allocator(), cy::replay_test::manifest());
    cy::replay::RejectReason why = cy::replay::RejectReason::None;
    const cy::Status read = cy::replay::read_log(
        cy::Span<const cy::u8>(reinterpret_cast<const cy::u8*>(committed.data()), committed.size()),
        golden, why);
    CY_REQUIRE(read.has_value());
    CY_CHECK_EQ(golden.size(), expected.records);
    CY_CHECK_EQ(golden.hash(), expected.log_hash);

    // THE REPLAY. A fresh session, the recording's producer topology, and the engine's own playback
    // path — `PlaybackDriver` writes into `CommandStream` producers exactly as a keyboard does, so
    // this is the same simulation path the recording took rather than a second one.
    cy::replay_test::ToySession playback;
    CY_REQUIRE(playback.build());
    cy::replay::PlaybackDriver driver(cy::replay_test::allocator(), golden);
    for (cy::u32 player = 0; player < cy::replay_test::ToySession::kPlayers; ++player) {
        CY_REQUIRE(
            driver.bind_participant(playback.participant_bits(player), playback.producer_of(player))
                .has_value());
    }

    for (u64 tick = 0; tick < kGoldenTicks; ++tick) {
        auto produced = driver.produce(playback.commands(), tick);
        CY_REQUIRE(produced.has_value());
        CY_CHECK_EQ(*produced, cy::replay_test::ToySession::kPlayers);
        CY_REQUIRE(playback.step(tick));
        // TICK BY TICK. A replay that diverged at tick 3 and converged again by tick 119 would pass
        // a final-hash-only comparison, and the per-tick file is what lets the failure name 3.
        CY_CHECK_EQ(playback.root_hash(), expected.hashes[static_cast<cy::usize>(tick)]);
    }
    CY_CHECK_EQ(driver.unbound_participants(), u64{0});
    CY_CHECK_EQ(driver.commands_produced(), kGoldenTicks * cy::replay_test::ToySession::kPlayers);
}

CY_TEST_CASE("golden: a replay from a build this one cannot reproduce is refused, not replayed") {
    // WHY THIS CASE IS IN THE GOLDEN SUITE RATHER THAN BESIDE `classify()`'s own unit tests. The
    // two cases above read a committed file and compare hashes; without this one, a golden artefact
    // left behind by an older engine would be replayed anyway and report a state-hash mismatch — a
    // stale expectation wearing a simulation regression's clothes. `replay-and-rollback` requires
    // the reason to distinguish a build mismatch from a damaged file, and that distinction is
    // exactly what keeps the two readable apart.
    cy::replay::CompatibilityManifest recorded = cy::replay_test::manifest();
    cy::replay::CompatibilityManifest current = cy::replay_test::manifest();
    current.engine_build = recorded.engine_build + 1;

    const cy::replay::MigrationWindow window;
    const cy::replay::CompatibilityVerdict verdict =
        cy::replay::classify(recorded, current, window);
    CY_CHECK(verdict.verdict != cy::replay::Compatibility::Reproducible);
    CY_CHECK(verdict.reason == cy::replay::RejectReason::BuildMismatch);
    // Never `Corrupt`, and this is the half a single "cannot play this" loses.
    CY_CHECK(verdict.reason != cy::replay::RejectReason::Corrupt);

    // And the artefact this suite actually commits is Reproducible against the build that wrote it.
    const cy::replay::CompatibilityVerdict same =
        cy::replay::classify(recorded, cy::replay_test::manifest(), window);
    CY_CHECK(same.verdict == cy::replay::Compatibility::Reproducible);
}
