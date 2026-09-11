#pragma once
// The crash artefact: the bounded ring, flushed into something loadable. M9 task 3.4.
//
// `replay-and-rollback` — "The crash replay buffer": builds "SHALL maintain a **rolling replay
// buffer** of a configured recent duration"; on a crash "the buffer SHALL be attachable to the
// report together with build identity, simulation point, recent state hashes, and the divergence
// capture if one exists"; and "The resulting artefact SHALL be **loadable to reproduce the final
// seconds of the session**."
//
// ================================================================================================
// THE LAST SENTENCE IS THE ONLY ONE WORTH TESTING, AND `to_log()` IS WHERE IT IS TESTED
// ================================================================================================
//
// "Attachable to the report" is satisfied by any pile of bytes. "Loadable to reproduce" is not:
// the artefact has to come back as a `RecordLog` that `PlaybackDriver` will drive, and the window
// it covers has to re-simulate to the hashes the session recorded on its way down. So `to_log()`
// exists, and `tests/test_crash.cpp` uses it the only way that means anything — it re-simulates the
// recovered window and compares the resulting state hash against the one the artefact carries.
//
// A crash artefact whose records decoded and whose replay produced a different world would pass
// every structural check and be worthless, which is exactly the failure this milestone is about.
//
// ================================================================================================
// THE RECENT HASHES ARE DERIVED FROM THE RING, NOT PASSED IN BESIDE IT
// ================================================================================================
//
// The requirement lists "recent state hashes" as a separate attachment, and the obvious
// implementation is a second array the caller keeps and hands over. That would be a second
// representation of something the ring already holds — `RecordKind::StateHash` records are in there
// — and two representations of the same thing drift, which is the failure M9 is named after.
// `assemble()` scans the flushed records instead. The cost is a walk of a bounded ring; the benefit
// is that the hashes in the artefact cannot disagree with the records in the artefact.
//
// ================================================================================================
// THE DURATION IS CONFIGURED IN SECONDS AND STORED IN RECORDS
// ================================================================================================
//
// "a configured recent duration" is what a person sets and "a ring of N records" is what the buffer
// is. `crash_ring_records()` converts, and it takes the records-per-tick estimate explicitly rather
// than guessing one: a four-player session at 60 Hz with a hash every fifteen ticks is a different
// number from a forty-player server, and a default that silently fitted one of them would give the
// other a window of the wrong length with nothing saying so.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/memory/array.h>
#include <cy/replay/log.h>
#include <cy/replay/readers.h>
#include <cy/replay/record.h>
#include <cy/replay/session.h>

namespace cy::replay {

/// How many records a ring needs to hold `seconds` of a session that produces `records_per_tick`.
///
/// Returns zero for a rate or a duration that cannot be served, which a caller must treat as "do
/// not enable the buffer" rather than as "use the default".
[[nodiscard]] u32 crash_ring_records(u64 seconds, u32 tick_rate_numerator,
                                     u32 tick_rate_denominator, u32 records_per_tick) noexcept;

/// Why the artefact was produced. "On a crash, assertion, or reported defect" — three, and they are
/// distinguished because a tester's "units stopped moving" and a segmentation fault want different
/// things read first.
enum class CrashTrigger : u8 {
    Crash = 0,
    Assertion,
    /// A tester pressed the button. `replay-and-rollback`'s own scenario.
    ReportedDefect,
};

const char* crash_trigger_name(CrashTrigger trigger) noexcept;

/// The most recent state hashes the artefact carries. Bounded: an artefact is a bug report
/// attachment, not an archive.
inline constexpr u32 kRecentHashes = 16;

/// The longest component or field name the artefact stores for a divergence. Names are metadata —
/// the identifiers beside them are what the report is about — so truncation costs readability and
/// never correctness.
inline constexpr u32 kArtefactNameLength = 32;

/// The artefact's first four bytes. Distinct from `kLogMagic`: a crash artefact handed to
/// `read_log` must be `Corrupt` rather than half-parsed.
inline constexpr u32 kCrashMagic = 0x5243'5943U;  // "CYCR" little-endian
inline constexpr u32 kCrashFormatVersion = 1;

/// The bug report's attachment.
class CrashArtefact {
public:
    explicit CrashArtefact(Allocator& allocator) noexcept : records_(allocator) {}

    CrashArtefact(const CrashArtefact&) = delete;
    CrashArtefact& operator=(const CrashArtefact&) = delete;

    /// Flush `ring` into the artefact and stamp it with the build identity and the simulation
    /// point.
    ///
    /// Refuses an empty ring: an artefact that reproduces nothing is worse than no artefact,
    /// because somebody will spend an afternoon loading it.
    [[nodiscard]] Status assemble(const CompatibilityManifest& manifest,
                                  const CrashReplayBuffer& ring, CrashTrigger trigger,
                                  determinism::SimulationPoint at) noexcept;

    /// "and the divergence capture if one exists". Optional in the strongest sense — an artefact
    /// without one is complete.
    [[nodiscard]] Status attach_divergence(const DivergenceReport& report) noexcept;

    [[nodiscard]] Status write(Array<u8>& out) const noexcept;

    /// Read one back. `why` distinguishes a damaged file from one this build cannot claim to
    /// reproduce, exactly as `read_log` does and for the same reason.
    [[nodiscard]] Status read(Span<const u8> bytes, RejectReason& why) noexcept;

    /// **The claim.** Rebuild a log the playback driver will drive.
    ///
    /// `out` must have been constructed with this artefact's manifest; the records are appended in
    /// the order the ring held them, which is the order the session produced them.
    [[nodiscard]] Status to_log(RecordLog& out) const noexcept;

    [[nodiscard]] const CompatibilityManifest& manifest() const noexcept { return manifest_; }
    [[nodiscard]] CrashTrigger trigger() const noexcept { return trigger_; }
    [[nodiscard]] determinism::SimulationPoint point() const noexcept { return at_; }
    [[nodiscard]] Span<const LogRecord> records() const noexcept { return records_.span(); }
    /// The window the artefact covers, in ticks. What a report says out loud: "the last 3.2 seconds
    /// of a fifty-minute session" is the honest description of a bounded ring.
    [[nodiscard]] u64 first_tick() const noexcept { return first_tick_; }
    [[nodiscard]] u64 last_tick() const noexcept { return last_tick_; }
    /// Records the ring overwrote before the artefact was taken. Non-zero is normal and is stated
    /// rather than implied.
    [[nodiscard]] u64 records_lost() const noexcept { return lost_; }

    [[nodiscard]] u32 recent_hash_count() const noexcept { return recent_hash_count_; }
    [[nodiscard]] u64 recent_hash(u32 index) const noexcept { return recent_hashes_[index]; }
    [[nodiscard]] u64 recent_hash_tick(u32 index) const noexcept {
        return recent_hash_ticks_[index];
    }

    [[nodiscard]] bool has_divergence() const noexcept { return has_divergence_; }
    [[nodiscard]] const DivergenceReport& divergence() const noexcept { return divergence_; }

    void clear() noexcept;

private:
    Array<LogRecord> records_;
    CompatibilityManifest manifest_;
    DivergenceReport divergence_;
    determinism::SimulationPoint at_;
    u64 recent_hashes_[kRecentHashes] = {};
    u64 recent_hash_ticks_[kRecentHashes] = {};
    /// Storage for the divergence's names, because `FieldDivergence` holds pointers into a schema
    /// that is gone by the time the artefact is read.
    char component_name_[kArtefactNameLength] = {};
    char field_name_[kArtefactNameLength] = {};
    u64 first_tick_ = 0;
    u64 last_tick_ = 0;
    u64 lost_ = 0;
    u32 recent_hash_count_ = 0;
    CrashTrigger trigger_ = CrashTrigger::Crash;
    bool has_divergence_ = false;
    bool assembled_ = false;
};

}  // namespace cy::replay
