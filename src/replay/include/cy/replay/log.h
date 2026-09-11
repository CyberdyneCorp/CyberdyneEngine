#pragma once
// The log: chunked over tick ranges, indexed, and reconstructible from its own bytes. M9 task 1.1.
//
// `replay-and-rollback` — "Replay contents": a replay is "a header, a compatibility manifest, a
// session descriptor with its seed and tick rate, an initial authoritative state, chunked command
// records, external result records, periodic checkpoints, an index, and optional presentation
// tracks", and it "SHALL be reconstructible from those alone". "Command records SHALL be **chunked
// over tick ranges and compressed**, not written per command, so that reading and seeking are
// efficient."
//
// ================================================================================================
// THE INDEX IS THE POINT, NOT THE COMPRESSION
// ================================================================================================
//
// "The replay index SHALL allow locating the nearest checkpoint at or before a target tick
// **without scanning the file**." That is what the chunk table below is for: one entry per tick
// range, carrying the range, where its records start, and the last checkpoint at or before its end.
// Seeking two hours into a session is then a binary search over the table and one chunk read.
//
// The compression is a deterministic run-length encoding over the chunk's bytes, and it is honest
// about being that rather than a general compressor: a record is a fixed-size struct that is mostly
// zeroes in its unused payload, which is the shape run-length encoding is actually good at, and a
// general compressor would be a dependency and a second thing whose output has to be bit-stable
// across versions. `tests/test_log.cpp` round-trips it and compares the compressed bytes of two
// identical logs, because a compressor that is not deterministic would silently break the file
// digest a lockstep session compares.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/profile.h>
#include <cy/core/memory/array.h>
#include <cy/replay/record.h>

namespace cy::replay {

/// What a replay depends on, recorded so playback can say whether it still holds.
///
/// `replay-and-rollback` — "Replay compatibility": "A replay SHALL record what it depends on:
/// engine and project build identity, gameplay and command schema versions, the plugin lockfile
/// hash, the content manifest hash, the determinism profile, and the tick rate."
struct CompatibilityManifest {
    u64 engine_build = 0;
    u64 project_build = 0;
    u64 plugin_lockfile_hash = 0;
    u64 content_manifest_hash = 0;
    /// The session's seed. Part of the descriptor rather than of the state, because every random
    /// draw in the session is a pure function of it.
    u64 session_seed = 0;
    /// The tick rate as an exact rational, never a float — `clock.h`'s own representation, and for
    /// its reason: 1/60 is not representable and a replay that accumulated it would drift.
    u32 tick_rate_numerator = 60;
    u32 tick_rate_denominator = 1;
    u16 command_schema_version = 1;
    u16 state_schema_version = 1;
    determinism::DeterminismProfile profile = determinism::DeterminismProfile::ReplayStable;
};

/// How far back playback will migrate. Declared by the project, because only the project knows
/// which of its own schema changes it wrote a migration for.
struct MigrationWindow {
    u16 oldest_command_schema = 1;
    u16 oldest_state_schema = 1;
};

/// `replay-and-rollback`: "Playback SHALL classify a replay as reproducible, compatible, or
/// incompatible."
enum class Compatibility : u8 {
    /// Identical build and content. The only class under which a bit-exact claim is made.
    Reproducible = 0,
    /// Within the declared migration window.
    Compatible,
    /// Outside it. Rejected with a reason.
    Incompatible,
};

/// Why a replay was refused. **`Corrupt` is deliberately in the same enumeration as the mismatches
/// and deliberately distinct from every one of them**: "the reason SHALL distinguish a build or
/// content mismatch from a damaged file", and a single "cannot play this" is what makes a support
/// queue unanswerable.
enum class RejectReason : u8 {
    None = 0,
    BuildMismatch,
    ContentMismatch,
    SchemaOutsideWindow,
    ProfileMismatch,
    TickRateMismatch,
    /// The bytes are not a replay, or are damaged. Never inferred from a mismatch.
    Corrupt,
};

const char* compatibility_name(Compatibility value) noexcept;
const char* reject_reason_name(RejectReason value) noexcept;

struct CompatibilityVerdict {
    Compatibility verdict = Compatibility::Reproducible;
    RejectReason reason = RejectReason::None;
    /// What differed, in a phrase. Never null.
    const char* detail = "";
};

/// Compare what a replay recorded against what this build is, under a declared window.
[[nodiscard]] CompatibilityVerdict classify(const CompatibilityManifest& recorded,
                                            const CompatibilityManifest& current,
                                            const MigrationWindow& window) noexcept;

/// Ticks per chunk. A power of two so the index's arithmetic is a shift, and small enough that
/// seeking reads one chunk rather than a minute of session.
inline constexpr u32 kChunkTicks = 64;

/// One chunk of the index.
struct ChunkIndexEntry {
    u64 first_tick = 0;
    u64 last_tick = 0;
    u32 first_record = 0;
    u32 record_count = 0;
    /// The tick of the last checkpoint at or before `last_tick`, or `kNoCheckpoint`. Carried in the
    /// index so that "the nearest checkpoint at or before a target tick" is a table lookup.
    u64 checkpoint_tick = kNoCheckpoint;
    u32 checkpoint_record = 0;

    static constexpr u64 kNoCheckpoint = ~0ULL;
};

/// The log, in memory: the records in order, the manifest they were recorded under, and the index.
///
/// Append-only and tick-monotonic. A record for a tick earlier than the last one is refused rather
/// than sorted in, because the order records arrive in *is* the order the simulation consumed them
/// and a log that reordered would be a log that had an opinion.
class RecordLog {
public:
    RecordLog(Allocator& allocator, const CompatibilityManifest& manifest) noexcept
        : records_(allocator), chunks_(allocator), manifest_(manifest) {}

    RecordLog(const RecordLog&) = delete;
    RecordLog& operator=(const RecordLog&) = delete;

    [[nodiscard]] Status append(const LogRecord& record) noexcept;

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(records_.size()); }
    [[nodiscard]] const LogRecord& at(u32 index) const noexcept { return records_[index]; }
    [[nodiscard]] Span<const LogRecord> records() const noexcept { return records_.span(); }
    [[nodiscard]] const CompatibilityManifest& manifest() const noexcept { return manifest_; }
    [[nodiscard]] Allocator& allocator() const noexcept { return records_.allocator(); }

    [[nodiscard]] u32 chunk_count() const noexcept { return static_cast<u32>(chunks_.size()); }
    [[nodiscard]] const ChunkIndexEntry& chunk(u32 index) const noexcept { return chunks_[index]; }

    /// The index of the first record at or after `tick`, found through the index rather than by a
    /// scan. `size()` when there is none.
    [[nodiscard]] u32 lower_bound(u64 tick) const noexcept;

    /// The nearest checkpoint record at or before `tick`. False when there is none, which is the
    /// honest answer for a seek into a session's first chunk.
    [[nodiscard]] bool nearest_checkpoint(u64 tick, LogRecord& out) const noexcept;

    /// A value identity over the whole log. Two sessions that produced the same record produce the
    /// same number. What a desync report compares first, and what the replay's own bytes are
    /// checked against on load.
    [[nodiscard]] u64 hash() const noexcept;

    void clear() noexcept;

private:
    Array<LogRecord> records_;
    Array<ChunkIndexEntry> chunks_;
    CompatibilityManifest manifest_;
    u64 last_tick_ = 0;
    bool any_ = false;
};

/// The file's first four bytes. A magic rather than a version alone, so that a file that is not a
/// replay is `Corrupt` and a replay from another build is a mismatch.
inline constexpr u32 kLogMagic = 0x59504C52U;  // "RLPY"
inline constexpr u32 kLogFormatVersion = 1;

/// Serialise header, manifest, index and compressed chunks. Deterministic: the same log produces
/// the same bytes, which `tests/test_log.cpp` checks by encoding twice and comparing.
[[nodiscard]] Status write_log(const RecordLog& log, Array<u8>& out) noexcept;

/// Read a log back. On refusal `why` says whether the bytes were damaged or the replay was written
/// by something this build cannot claim to reproduce.
[[nodiscard]] Status read_log(Span<const u8> bytes, RecordLog& out, RejectReason& why) noexcept;

/// The deterministic run-length encoding the chunks use. Exposed because the round-trip is worth
/// testing on its own, and because a caller writing its own container wants the same bytes.
[[nodiscard]] Status compress(Span<const u8> input, Array<u8>& out) noexcept;
[[nodiscard]] Status decompress(Span<const u8> input, usize expected_size, Array<u8>& out) noexcept;

}  // namespace cy::replay
