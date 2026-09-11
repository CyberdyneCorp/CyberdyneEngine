#pragma once
// THE ONE RECORD. M9 tasks 1.1 and 1.5.
//
// ================================================================================================
// WHY THIS FILE IS THE WHOLE OF THE MILESTONE'S SUBTITLE
// ================================================================================================
//
// M9 is "one command log, read five ways", and the failure it guards against is the obvious one:
// replay grows a record, replication grows a second, the crash buffer grows a third, and the three
// drift until a replay of a networked session is a different program from the session.
// `replay-and-rollback` says it in one sentence — "A second representation of participant intent
// SHALL NOT exist, because two representations of the same thing drift" — and states it over three
// readers while the engine has five.
//
// So: **`LogRecord` is the record, and a reader that declares its own does not compile.** The
// enumeration below names all five, `bind_reader<R>()` will not instantiate for a type whose
// `record_type` is anything else, and `log_readers()` is the list a test compares against
// `LogReader::Count`. Adding a sixth reader without declaring it fails that test by name.
//
// This is the opposite of M8.b's IR finding and it is not a contradiction. A graph IR is an
// *execution form*, and five consumers execute differently, so five lowerings was the right answer
// there. A command log is a *record of what happened*, and five records of what happened that
// disagree is the bug.
//
// ================================================================================================
// WHY THE RECORD IS FIXED-SIZE, AND WHAT IT COSTS
// ================================================================================================
//
// Every record is the same number of bytes whatever its kind, which means the in-memory log is an
// array, the crash ring is a ring of values with no allocation, and seeking within a chunk is
// arithmetic. It costs: a state-hash record carries a `Command`'s worth of bytes it does not use,
// so a log that is mostly hashes is about twice the size of one that packed per kind. That is the
// trade and it is deliberate — the crash replay buffer is the reader that decides it, because a
// bounded ring written from a signal handler cannot allocate and cannot afford a variable stride.
//
// ================================================================================================
// SERIALISATION IS FIELD BY FIELD, NEVER A MEMCPY OF THE STRUCT
// ================================================================================================
//
// A `memcpy` of a struct writes its padding, and padding is whatever the last thing to occupy those
// bytes left behind. Two runs that agree about every value would then produce different bytes and a
// different file digest, which is precisely the class of defect this milestone exists to detect.
// `encode()`/`decode()` below write each field at a fixed offset, little-endian, and
// `kEncodedRecordSize` is a constant the tests compare against.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/memory/array.h>
#include <cy/gameplay/command.h>

#include <concepts>
#include <type_traits>

namespace cy::replay {

/// What one record is about.
///
/// Five kinds and not one per reader: the kinds are what the *session* produced, and every reader
/// sees all of them. A playback cursor skips hashes; a validator skips nothing.
enum class RecordKind : u8 {
    /// A committed gameplay command, exactly as the simulation consumed it.
    Command = 0,
    /// Something authoritative the simulation consumed and did not compute — see external.h.
    ExternalResult,
    /// A checkpoint was taken at this point; `value` addresses it in the checkpoint store.
    Checkpoint,
    /// The authoritative state hash at this point. What a lockstep peer compares and what the
    /// validator descends from.
    StateHash,
    /// A presentation-side effect was realised at this point — the side-effect ledger's entry.
    Effect,
};

const char* record_kind_name(RecordKind kind) noexcept;

/// The largest inline payload a non-command record carries. An external result that needs more is
/// carrying content rather than an outcome, and the content belongs in an asset it names — the same
/// argument `gameplay::kMaxCommandPayload` makes, for the same reason.
inline constexpr u16 kMaxRecordPayload = 48;

/// **The record.** One type, five readers.
struct LogRecord {
    /// A value identity for the type itself. Every reader's binding carries it, and a reader that
    /// substituted a record of its own would carry a different one — which is what makes
    /// `tests/test_one_record.cpp` a check rather than a review.
    ///
    /// Bumped when the encoding below changes, which is also the replay format's version.
    static constexpr u32 kRecordTypeId = 0x43594C31U;  // "CYL1"

    RecordKind kind = RecordKind::Command;

    /// **The moment, as an (epoch, tick) pair rather than a tick.** `simulation-and-determinism`:
    /// rollback moves the tick backwards, so a tick alone does not name a moment. Two records at
    /// the same tick in different epochs are different records and the ledger treats them so.
    determinism::Epoch epoch;
    u64 tick = 0;

    /// Ordering within a tick. For a command this is the stream's merge sequence, which is what
    /// `replay-and-rollback` asks for — "Commands SHALL carry a per-participant sequence number
    /// within a tick, so that ordering is defined, duplicates are detectable, and gaps are
    /// reportable". For every other kind it is the order the session produced them in.
    u32 sequence = 0;

    /// Command records: the command as committed, provenance included and deciding nothing.
    /// Meaningless for every other kind, and `encode()` still writes it — a fixed-size record.
    gameplay::Command command;

    /// A stable identity for what the record is about: the external source, the effect, the
    /// checkpoint. Zero for a command, which is identified by the command itself.
    u64 subject = 0;

    /// The record's number: a state hash, a checkpoint's offset, an effect's instance. Zero where
    /// there is none.
    u64 value = 0;

    u16 payload_size = 0;
    u8 payload[kMaxRecordPayload] = {};

    [[nodiscard]] determinism::SimulationPoint point() const noexcept {
        return determinism::SimulationPoint{epoch, tick};
    }
};

/// One record on the wire or in a file. A constant rather than `sizeof(LogRecord)`: the struct has
/// padding and the encoding does not, and the two must be allowed to differ.
inline constexpr u32 kEncodedRecordSize = 192;

/// Append `record` to `out` as exactly `kEncodedRecordSize` bytes, little-endian, no padding.
[[nodiscard]] Status encode(const LogRecord& record, Array<u8>& out) noexcept;

/// Read one record from the front of `bytes`. Refuses a buffer shorter than one record rather than
/// reading past it.
[[nodiscard]] Status decode(Span<const u8> bytes, LogRecord& out) noexcept;

/// Fold a record into a value identity, field by field and **without provenance**.
///
/// Provenance is excluded for the same reason `gameplay::CommandLog::hash()` excludes it: a
/// replay's records carry `Replay` provenance where the originals carried `Human`, and a digest
/// that included it would make every replay differ from the session it reproduces. The first thing
/// anyone would then do is make the replay lie about where its commands came from.
[[nodiscard]] u64 record_hash(u64 accumulator, const LogRecord& record) noexcept;

// --- The five readers ---------------------------------------------------------------------------

/// Every reader of the command log, enumerated. `replay-and-rollback`'s ADDED requirement: "The
/// engine SHALL therefore enumerate every reader of the command log, and the claim that they share
/// one record type SHALL be checked by a test rather than by review."
enum class LogReader : u8 {
    /// Playback and seeking against checkpoints.
    Playback = 0,
    /// Rollback re-simulation from a checkpoint.
    Rollback,
    /// Replication's input path — the modes that send inputs. `src/networking/` drives this cursor;
    /// it does not declare one of its own, and `CY_NETWORKING=OFF` removes the driver rather than
    /// the record.
    ReplicationInput,
    /// The bounded ring flushed into the crash artefact.
    CrashBuffer,
    /// The divergence validator's window.
    Validator,
    Count,
};

inline constexpr u32 kLogReaderCount = static_cast<u32>(LogReader::Count);

const char* log_reader_name(LogReader reader) noexcept;

/// A type that reads the command log. **The check's compile-time half**, asserted once per reader
/// in `record.cpp`.
template <class R>
concept ReadsTheOneRecord =
    requires { typename R::record_type; } && std::same_as<typename R::record_type, LogRecord>;

/// A type that declares *a* record. Deliberately weaker than `ReadsTheOneRecord`.
///
/// `bind_reader<>()` is constrained on THIS and not on the stronger concept, and the reason is the
/// whole design of the check. If binding required the record to be `LogRecord`, then substituting a
/// reader's record would be a compile error and the **runtime** case in
/// `tests/test_one_record.cpp` could never be reached — it would be a check with no way to fail,
/// which is the exact defect this project's gates have found eighteen times. Two mutations at two
/// depths instead: change a reader's `record_type` and the `static_assert` list in `record.cpp`
/// stops the build; remove that assertion too and the runtime case goes red naming the reader.
template <class R>
concept DeclaresARecord = requires {
    typename R::record_type;
    { R::record_type::kRecordTypeId } -> std::convertible_to<u32>;
};

/// What one reader declares about the record it reads.
struct LogReaderBinding {
    LogReader reader = LogReader::Playback;
    /// The reader's own type name, for the failure message.
    const char* name = "";
    /// Taken from the reader's `record_type`, not restated. A reader that substituted its own
    /// record carries that record's identity here, and the test names it.
    u32 record_type_id = 0;
    u32 record_size = 0;
};

template <class R>
    requires DeclaresARecord<R>
[[nodiscard]] constexpr LogReaderBinding bind_reader(LogReader which, const char* name) noexcept {
    return LogReaderBinding{which, name, R::record_type::kRecordTypeId,
                            static_cast<u32>(sizeof(typename R::record_type))};
}

/// The five bindings, in `LogReader` order. Read by `tests/test_one_record.cpp`, which fails when
/// the list is short, out of order, or carries a record identity that is not `LogRecord`'s.
[[nodiscard]] Span<const LogReaderBinding> log_readers() noexcept;

}  // namespace cy::replay
