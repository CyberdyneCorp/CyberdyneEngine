#pragma once
// External results: everything the simulation consumed that it did not compute. M9 task 1.2.
//
// `replay-and-rollback` — "External results": authoritative outcomes that are **not reproducible by
// simulation** — "a service response, a matchmaking assignment, a machine-learning inference
// result, a secure random value, a real-world date" — are recorded with their simulation point and
// type; "during replay and re-simulation, the recorded result SHALL be consumed and the external
// source SHALL **NOT** be invoked"; and "Failing to record a consumed external source SHALL be
// detectable: replay validation SHALL report an external consumption with no corresponding record."
//
// ================================================================================================
// ONE DOOR, TWO MODES, AND THE SECOND ONE CANNOT REACH THE SOURCE
// ================================================================================================
//
// `consume()` is the only way an authoritative system gets an external value, and it takes the
// producer as an argument rather than the system calling it directly. That is the whole mechanism:
// in `Replaying` mode the producer is **never invoked** — not "invoked and ignored", not "invoked
// and compared" — and `produce_invocations()` is the number a test asserts is zero, because
// "SHALL NOT be invoked" is a negative claim and a counter is how a negative claim is checked.
//
// The detection is the other half and it falls out of the same door. A `consume()` in `Replaying`
// mode that finds no record for its `(source, point)` is an external consumption the recording did
// not know about: it is counted, the source is named, and `Report::first_unrecorded` is what the
// replay-validation diagnostic quotes. Without the single door there would be nothing to count.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/memory/array.h>
#include <cy/replay/log.h>
#include <cy/replay/record.h>

namespace cy::replay {

/// What kind of thing was consumed. Recorded with the value, because "a service said 47" and "the
/// clock said 47" fail differently and a replay that could not tell them apart could not report
/// either.
enum class ExternalKind : u8 {
    ServiceResponse = 0,
    MatchmakingAssignment,
    Inference,
    SecureRandom,
    WallClockDate,
    Other,
};

const char* external_kind_name(ExternalKind kind) noexcept;

/// A source an authoritative system declares it consumes.
///
/// "An authoritative system that consumes an external source SHALL declare it, so that a session
/// under `ReplayStable` or stricter records what it must." The declaration is what makes an
/// undeclared consumption a refusal rather than a silent recording.
struct ExternalSource {
    /// A stable identity. Never a registration index: a replay outlives the process that wrote it.
    u64 id = 0;
    const char* name = "";
    ExternalKind kind = ExternalKind::Other;
};

/// Recording or replaying. There is no third mode, and in particular no "replay but call the source
/// anyway to compare": that would make the replay's behaviour depend on the service being up.
enum class ExternalMode : u8 {
    Recording = 0,
    Replaying,
};

/// Produce the value. Invoked in `Recording` and never in `Replaying`.
using ProduceFn = Status (*)(void* user, Array<u8>& out) noexcept;

/// What one session's external results amount to.
struct ExternalReport {
    u32 sources_declared = 0;
    u32 recorded = 0;
    u32 consumed = 0;
    /// `Replaying` consumptions with no record. The detection the requirement asks for.
    u32 unrecorded_consumptions = 0;
    /// Records the replay never consumed. Not an error — a replay may stop early — and reported
    /// because a replay that consumed a tenth of what was recorded is a replay that diverged.
    u32 unconsumed_records = 0;
    /// The first source consumed without a record, for the diagnostic. "" for none.
    const char* first_unrecorded = "";
};

/// The one door.
class ExternalResults {
public:
    ExternalResults(Allocator& allocator, ExternalMode mode) noexcept
        : sources_(allocator), records_(allocator), consumed_(allocator), mode_(mode) {}

    ExternalResults(const ExternalResults&) = delete;
    ExternalResults& operator=(const ExternalResults&) = delete;

    [[nodiscard]] Status declare(const ExternalSource& source) noexcept;
    [[nodiscard]] const ExternalSource* find(u64 id) const noexcept;
    [[nodiscard]] ExternalMode mode() const noexcept { return mode_; }

    /// Take the recorded external results out of a log. The replay side's input.
    [[nodiscard]] Status adopt(const RecordLog& log) noexcept;

    /// **Consume an external value.**
    ///
    /// `Recording`: invokes `produce`, appends an `ExternalResult` record to `log`, and returns the
    /// value. `Replaying`: returns the recorded value for `(source, at)` and does not touch
    /// `produce` at all; a missing record is counted, named, and refused.
    ///
    /// Refuses an undeclared source in both modes, and a value longer than `kMaxRecordPayload` —
    /// an external result that large is content rather than an outcome.
    [[nodiscard]] Status consume(u64 source, determinism::SimulationPoint at, ProduceFn produce,
                                 void* user, RecordLog& log, Array<u8>& value) noexcept;

    /// How many times a producer has actually been called. Zero is the claim `Replaying` makes, and
    /// this is how a test checks a negative.
    [[nodiscard]] u32 produce_invocations() const noexcept { return produce_invocations_; }

    [[nodiscard]] const ExternalReport& report() const noexcept { return report_; }
    /// Recompute `unconsumed_records` and return the report. Called when a replay ends.
    [[nodiscard]] const ExternalReport& finish() noexcept;

    void clear() noexcept;

private:
    struct Recorded {
        u64 source = 0;
        determinism::Epoch epoch;
        u64 tick = 0;
        u16 size = 0;
        u8 bytes[kMaxRecordPayload] = {};
        bool consumed = false;
    };

    [[nodiscard]] Recorded* match(u64 source, determinism::SimulationPoint at) noexcept;

    Array<ExternalSource> sources_;
    Array<Recorded> records_;
    /// Consumptions, in order, for a diagnostic that wants to say what the replay asked for.
    Array<u64> consumed_;
    ExternalReport report_;
    ExternalMode mode_ = ExternalMode::Recording;
    u32 produce_invocations_ = 0;
};

}  // namespace cy::replay
