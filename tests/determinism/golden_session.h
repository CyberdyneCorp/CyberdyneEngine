#pragma once
// The session both determinism suites drive, and the committed form of its record. M10 task 6.4.
//
// ================================================================================================
// WHY THE SUITE REACHES INTO src/replay/tests/ FOR ITS SIMULATION
// ================================================================================================
//
// `testing-and-quality` gives this kind one subject — "reproducibility of simulation and
// replication" — and a golden replay is worth exactly as much as the thing it replays. A fixture
// invented here would be a counter, and a committed hash of a counter proves that addition is
// stable. `src/replay/tests/sim.h` is already the engine's own pieces wired the way a game wires
// them: a real `ecs::World`, a real `gameplay::GameSession` with four participants, one
// `CommandStream` producer per participant, real `determinism::StateCodec`s and a real
// `StateProvider`. So this suite includes it rather than growing a second one, and
// tests/determinism/CMakeLists.txt puts that directory on the include path with the reason written
// beside it.
//
// ================================================================================================
// WHAT MAKES THIS A GOLDEN TEST AND NOT A SECOND COPY OF test_bitexact.cpp
// ================================================================================================
//
// `src/replay/tests/test_bitexact.cpp` records a session and replays it IN THE SAME PROCESS. That
// proves the replay machinery reproduces whatever the simulation did today; it cannot notice that
// the simulation started doing something else, because both halves changed together.
//
// `testing-and-quality` asks for the other thing: "golden replays: recorded sessions with committed
// hashes, replayed in CI, so a regression and a deliberate behaviour change are distinguishable".
// The expectation therefore lives in the repository — `golden/toy-session-v1.cyreplay` and
// `golden/toy-session-v1.hashes` — and a change in the simulation's behaviour shows up as a diff a
// reviewer reads, with a hash per tick so the diff names the first tick that moved.

#include "sim.h"

#include <cy/core/memory/array.h>
#include <cy/replay/log.h>
#include <cy/replay/session.h>
#include <cy/test/test.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace cy::determinism_test {

using cy::replay_test::ToySession;

/// The recorded session's length, and how often it checkpoints. Committed constants: changing
/// either one changes the artefact, which is the point — the artefact is the expectation.
inline constexpr cy::u64 kGoldenTicks = 120;
inline constexpr cy::u64 kGoldenCheckpointEvery = 20;

/// One recording: the log's bytes, and the state hash the session stood at after every tick.
struct Recording {
    cy::Array<cy::u8> bytes;
    cy::Array<cy::u64> hashes;
    cy::u64 log_hash = 0;
    cy::u32 records = 0;

    explicit Recording(cy::Allocator& allocator) noexcept : bytes(allocator), hashes(allocator) {}
};

/// Run the session live for `ticks`, recording it. Returns false rather than asserting so a case
/// can say which stage failed; every call here is already `[[nodiscard]]`.
///
/// The length is a parameter because the two suites want opposite things from it. The golden case
/// wants a session long enough to cross several chunks and several checkpoints, so a behaviour
/// change has somewhere to show up; the fuzz sweeps decode the whole artefact once per byte of it,
/// so their cost is QUADRATIC in the length and a 120-tick file would spend the suite's whole 10 s
/// budget proving what eight ticks prove.
[[nodiscard]] inline bool record_session(cy::u64 ticks, cy::u64 checkpoint_every,
                                         Recording& out) noexcept {
    cy::replay::RecordLog log(cy::replay_test::allocator(), cy::replay_test::manifest());
    ToySession live;
    if (!live.build()) {
        return false;
    }
    cy::replay::SessionRecorder recorder(log);
    recorder.attach(live.commands());
    for (cy::u64 tick = 0; tick < ticks; ++tick) {
        recorder.set_point(cy::determinism::SimulationPoint{cy::determinism::Epoch{}, tick});
        if (!live.live_tick(tick)) {
            cy::replay::SessionRecorder::detach(live.commands());
            return false;
        }
        // A checkpoint record carries the store's address and nothing else, so the golden log holds
        // the seek structure a replay of it has to honour without this suite owning a snapshot
        // ring.
        if (tick % checkpoint_every == 0 && !recorder.record_checkpoint(tick).has_value()) {
            cy::replay::SessionRecorder::detach(live.commands());
            return false;
        }
        const cy::u64 hash = live.root_hash();
        if (!out.hashes.push_back(hash).has_value() ||
            !recorder.record_state_hash(hash).has_value()) {
            cy::replay::SessionRecorder::detach(live.commands());
            return false;
        }
    }
    cy::replay::SessionRecorder::detach(live.commands());
    if (recorder.dropped() != 0) {
        return false;
    }
    out.log_hash = log.hash();
    out.records = log.size();
    return cy::replay::write_log(log, out.bytes).has_value();
}

/// The committed artefact's session: `kGoldenTicks` ticks, checkpointed every
/// `kGoldenCheckpointEvery`.
[[nodiscard]] inline bool record_golden_session(Recording& out) noexcept {
    return record_session(kGoldenTicks, kGoldenCheckpointEvery, out);
}

/// The session the fuzz sweeps damage. Short on purpose — see `record_session()`.
inline constexpr cy::u64 kFuzzTicks = 8;
inline constexpr cy::u64 kFuzzCheckpointEvery = 4;

// --- The committed artefact
// -----------------------------------------------------------------------

/// The hashes file, as text. One tick per line, so a behaviour change is a diff that names the
/// first tick that moved rather than one number that moved.
[[nodiscard]] inline std::string format_hashes(const Recording& recording) {
    std::string text =
        "# The committed state hash of tests/determinism/golden/toy-session-v1.cyreplay, per "
        "tick.\n"
        "# Regenerate deliberately: CY_DETERMINISM_RECORD_GOLDEN=1 ctest -R "
        "determinism.golden_replay\n"
        "# A diff here is a behaviour change in the simulation, the codecs or the hash tree, and "
        "it\n"
        "# is reviewed as one. `testing-and-quality`: a regression and a deliberate change are\n"
        "# distinguishable.\n";
    char line[128];
    (void)std::snprintf(line, sizeof(line), "records %u\nlog-hash %016llx\nticks %llu\n",
                        recording.records, static_cast<unsigned long long>(recording.log_hash),
                        static_cast<unsigned long long>(recording.hashes.size()));
    text += line;
    for (cy::usize tick = 0; tick < recording.hashes.size(); ++tick) {
        (void)std::snprintf(line, sizeof(line), "%llu %016llx\n",
                            static_cast<unsigned long long>(tick),
                            static_cast<unsigned long long>(recording.hashes[tick]));
        text += line;
    }
    return text;
}

/// What `format_hashes()` wrote, read back. Empty `hashes` means the file could not be parsed,
/// which a case reports rather than treating as "no ticks to compare" — a golden file that silently
/// parsed to nothing would make every comparison below vacuous.
struct GoldenHashes {
    cy::u32 records = 0;
    cy::u64 log_hash = 0;
    std::vector<cy::u64> hashes;
};

[[nodiscard]] inline GoldenHashes parse_hashes(const std::string& text) {
    GoldenHashes parsed;
    cy::usize start = 0;
    while (start < text.size()) {
        const cy::usize end = text.find('\n', start);
        const std::string line = text.substr(start, end == std::string::npos ? end : end - start);
        start = end == std::string::npos ? text.size() : end + 1;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        unsigned long long first = 0;
        unsigned long long second = 0;
        if (std::sscanf(line.c_str(), "records %llu", &first) == 1) {
            parsed.records = static_cast<cy::u32>(first);
        } else if (std::sscanf(line.c_str(), "log-hash %llx", &first) == 1) {
            parsed.log_hash = static_cast<cy::u64>(first);
        } else if (std::sscanf(line.c_str(), "ticks %llu", &first) == 1) {
            parsed.hashes.reserve(static_cast<cy::usize>(first));
        } else if (std::sscanf(line.c_str(), "%llu %llx", &first, &second) == 2) {
            // The tick is the index, checked rather than assumed: a file whose lines were reordered
            // would otherwise compare a tick against another tick's hash and pass.
            if (first != parsed.hashes.size()) {
                return GoldenHashes{};
            }
            parsed.hashes.push_back(static_cast<cy::u64>(second));
        }
    }
    return parsed;
}

/// The committed artefact's directory, compiled in by tests/determinism/CMakeLists.txt rather than
/// discovered at run time. A test that guesses where its expectation lives passes vacuously when it
/// guesses wrong, which is `tests/integration/test_wrapper_seam.cpp`'s rule for the same reason.
[[nodiscard]] inline std::string golden_path(const char* name) {
    return std::string(CY_DETERMINISM_GOLDEN_DIR) + "/" + name;
}

inline constexpr const char* kGoldenLogName = "toy-session-v1.cyreplay";
inline constexpr const char* kGoldenHashesName = "toy-session-v1.hashes";

/// Whether this run is regenerating the expectation instead of checking against it.
[[nodiscard]] inline bool recording_requested() noexcept {
    const char* value = std::getenv("CY_DETERMINISM_RECORD_GOLDEN");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

}  // namespace cy::determinism_test
