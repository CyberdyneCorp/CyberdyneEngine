#pragma once
// Prediction, reconciliation policy, and lag compensation. M9 task 4.5.
//
// ================================================================================================
// THE MECHANISM IS `replay-and-rollback`'s. WHAT IS HERE IS THE POLICY
// ================================================================================================
//
// `networking-and-replication` — "Rollback and reconciliation primitives" — draws the line itself:
// "the mechanism itself — the snapshot ring, restore, re-simulation from the command log, and the
// side-effect ledger that prevents effects being realised twice — is defined in
// `replay-and-rollback`. This capability owns the **networking policy** over that mechanism: when
// to roll back, the tolerance for divergence between predicted and authoritative state, correction
// smoothing, and the response when a correction predates the window."
//
// So there is no snapshot ring in this file, no restore, and no ledger. There is a decision —
// `PredictionLedger::compare()` returns Matched, Correct or Resynchronise — and the numbers behind
// it. `tests/test_prediction.cpp` proves the omission the only way that means anything: the module
// does not link `cy::replay`'s ledger types into any signature here, so a second suppression cannot
// grow in this file. "Effects are not realised twice ... and networking SHALL NOT implement its own
// suppression."
//
// ================================================================================================
// THE INPUT BUFFER HOLDS `replay::LogRecord` AND NOT A RECORD OF ITS OWN
// ================================================================================================
//
// This is the milestone's subtitle as a type. `replay-and-rollback`: "A second representation of
// participant intent SHALL NOT exist, because two representations of the same thing drift." A
// client's buffer of unacknowledged inputs is participant intent, so it is the session's record —
// `LogRecord`, the one `PlaybackCursor`, `RollbackCursor`, `ReplicationInputCursor`,
// `CrashReplayBuffer` and the validator all read. `using record_type = LogRecord;` is declared
// below for the same reason those five declare it.
//
// ================================================================================================
// LAG COMPENSATION REWINDS PROXIES, AND SAYS SO
// ================================================================================================
//
// "Lag compensation SHALL be implemented over a retained history of **collision proxies** —
// simplified shapes and transforms per tick — rather than full physics state, and the resulting
// accuracy difference from a live query SHALL be documented." `ProxyHistory` stores a sphere per
// entity per tick and `rewind()` hands them back; `kProxyAccuracyNote` is the documented
// difference, carried as a string beside the API rather than left in a design document nobody reads
// at the call site.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/networking/authority.h>
#include <cy/replay/record.h>

namespace cy::net {

/// The client's unacknowledged inputs, in tick order.
///
/// `networking-and-replication`: "buffer inputs with tick numbers, apply them immediately to the
/// predicted state, send them to the server, and retain them until acknowledged".
class InputBuffer {
public:
    /// The one record. See the header comment, and `replay::ReadsTheOneRecord`.
    using record_type = replay::LogRecord;

    explicit InputBuffer(Allocator& allocator) noexcept : records_(allocator) {}

    InputBuffer(const InputBuffer&) = delete;
    InputBuffer& operator=(const InputBuffer&) = delete;

    /// Retain one input. Refuses a record that is not a command: an external result or a state hash
    /// in an input buffer would be re-simulated as intent.
    [[nodiscard]] Status retain(const replay::LogRecord& record) noexcept;

    /// Drop everything at or before `tick`. What an acknowledgement does.
    [[nodiscard]] u32 acknowledge(u64 tick) noexcept;

    /// Every retained input for `[from_tick, to_tick]`, appended to `out` in recorded order. What a
    /// re-simulation replays.
    [[nodiscard]] Status replay_range(u64 from_tick, u64 to_tick,
                                      Array<replay::LogRecord>& out) const noexcept;

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(records_.size()); }
    [[nodiscard]] u64 oldest_tick() const noexcept;
    [[nodiscard]] u64 newest_tick() const noexcept;
    [[nodiscard]] u64 acknowledged_through() const noexcept { return acknowledged_; }

    void clear() noexcept;

private:
    Array<replay::LogRecord> records_;
    u64 acknowledged_ = 0;
};

/// What the client should do about one authoritative update.
enum class ReconciliationVerdict : u8 {
    /// Prediction and authority agree within tolerance. Nothing happens, which is the common case
    /// and the one a policy must not make expensive.
    Matched = 0,
    /// They differ. Restore the authoritative state for that tick and re-simulate the buffered
    /// inputs — through `replay`'s mechanism, not through this module.
    Correct,
    /// The update predates the rollback window. `networking-and-replication`: "a full
    /// resynchronisation SHALL occur, reported as such."
    Resynchronise,
    /// Nothing was predicted for that tick, so there is nothing to compare. Distinct from `Matched`
    /// because a policy that reported "matched" for a tick it never simulated would be a policy
    /// whose success rate is meaningless.
    NotPredicted,
};

const char* reconciliation_verdict_name(ReconciliationVerdict verdict) noexcept;

struct ReconciliationPolicy {
    /// Ticks of prediction retained. An authoritative update older than this cannot be corrected,
    /// only resynchronised.
    u32 window_ticks = 64;
    /// Ticks over which a visual correction is blended. The simulation is corrected immediately;
    /// only presentation is smoothed.
    u32 smoothing_ticks = 8;
    /// A hash comparison is exact by construction, so the tolerance applies to the *magnitude* the
    /// caller reports beside it — a squared distance in the caller's own units. Zero means any
    /// difference corrects.
    u64 tolerance = 0;
};

/// One tick's prediction, kept so an authoritative update can be compared against it.
struct PredictedTick {
    u64 tick = 0;
    /// The authoritative state hash the client's own simulation produced for that tick.
    u64 state_hash = 0;
};

struct ReconciliationReport {
    u64 compared = 0;
    u64 matched = 0;
    u64 corrected = 0;
    u64 resynchronised = 0;
    u64 not_predicted = 0;
    /// The largest magnitude a correction has carried. What "prediction error magnitude" is.
    u64 largest_error = 0;
    u64 last_corrected_tick = 0;
};

/// The client's prediction record, and the decision over it.
class PredictionLedger {
public:
    explicit PredictionLedger(Allocator& allocator) noexcept : predicted_(allocator) {}

    PredictionLedger(const PredictionLedger&) = delete;
    PredictionLedger& operator=(const PredictionLedger&) = delete;

    void set_policy(const ReconciliationPolicy& policy) noexcept { policy_ = policy; }
    [[nodiscard]] const ReconciliationPolicy& policy() const noexcept { return policy_; }

    /// Record what the client predicted for `tick`. Evicts anything outside the window, which is
    /// what makes the window bounded rather than aspirational.
    [[nodiscard]] Status predict(u64 tick, u64 state_hash) noexcept;

    /// Compare an authoritative state hash for `tick` against what was predicted. `magnitude` is
    /// the caller's own measure of how far apart they are, compared against the tolerance; a caller
    /// with no such measure passes `1`, which corrects on any difference.
    [[nodiscard]] ReconciliationVerdict compare(u64 tick, u64 authoritative_hash,
                                                u64 magnitude) noexcept;

    [[nodiscard]] const ReconciliationReport& report() const noexcept { return report_; }
    [[nodiscard]] u32 retained() const noexcept { return static_cast<u32>(predicted_.size()); }
    [[nodiscard]] u64 newest_predicted_tick() const noexcept { return newest_; }

    void clear() noexcept;

private:
    Array<PredictedTick> predicted_;
    ReconciliationPolicy policy_{};
    ReconciliationReport report_{};
    u64 newest_ = 0;
};

/// A visual correction, blended rather than snapped.
///
/// `networking-and-replication`: "the visual position SHALL converge over the smoothing interval
/// while the simulation state is corrected immediately". The simulation is not this class's
/// business; what it produces is the blend weight for a tick, in per cent, so no floating point
/// crosses the boundary between a correction's policy and its presentation.
class CorrectionSmoother {
public:
    void begin(u64 tick, u32 smoothing_ticks) noexcept;

    /// The weight of the *corrected* value at `tick`, 0 to 100. 100 once the interval has elapsed,
    /// and 100 immediately when the interval is zero.
    [[nodiscard]] u32 weight_at(u64 tick) const noexcept;
    [[nodiscard]] bool active(u64 tick) const noexcept;
    [[nodiscard]] u64 corrections() const noexcept { return corrections_; }

private:
    u64 began_ = 0;
    u32 ticks_ = 0;
    bool running_ = false;
    u64 corrections_ = 0;
};

/// The documented accuracy difference of a proxy rewind against a live query. Carried here so that
/// the caller of `rewind()` reads it, rather than a design document nobody opens at the call site.
inline constexpr const char* kProxyAccuracyNote =
    "a rewound hit test is evaluated against a sphere proxy per entity per tick, not against the "
    "entity's full collision geometry: a shot that grazes a limb may hit the proxy and a shot "
    "through a gap in the silhouette may miss it. The proxy is the entity's bounding sphere, so "
    "the "
    "error is always in the direction of a more generous hit.";

/// One entity's collision proxy at one tick.
struct CollisionProxy {
    NetworkId id;
    i64 x = 0;
    i64 y = 0;
    i64 z = 0;
    i64 radius = 0;
};

/// Why a lag-compensated claim was refused.
enum class RewindRefusal : u8 {
    None = 0,
    /// Older than the retained window.
    OutsideWindow,
    /// Further back than the client's measured latency can justify. "the client's claimed tick
    /// validated against its measured latency, so the advantage available to a high-latency or
    /// malicious client is limited".
    ImplausibleForLatency,
    /// Ahead of the server's own tick.
    InTheFuture,
};

const char* rewind_refusal_name(RewindRefusal refusal) noexcept;

/// The server's retained proxy history.
class ProxyHistory {
public:
    ProxyHistory(Allocator& allocator, u32 window_ticks) noexcept;

    ProxyHistory(const ProxyHistory&) = delete;
    ProxyHistory& operator=(const ProxyHistory&) = delete;

    /// Record the proxies for one tick. Overwrites the ring slot that tick maps to, which is what
    /// makes the window bounded and the memory fixed after the first `window_ticks` ticks.
    [[nodiscard]] Status record(u64 tick, Span<const CollisionProxy> proxies) noexcept;

    /// The proxies as they were at `tick`, appended to `out`. Refuses a tick outside the window or
    /// implausible for the claiming peer's latency.
    [[nodiscard]] RewindRefusal rewind(u64 tick, u64 now_tick, u32 peer_latency_ticks,
                                       Array<CollisionProxy>& out) const noexcept;

    [[nodiscard]] u32 window_ticks() const noexcept { return window_; }
    [[nodiscard]] u64 recorded_ticks() const noexcept { return recorded_; }
    [[nodiscard]] u64 refusals() const noexcept { return refusals_; }

private:
    struct Slot {
        u64 tick = 0;
        u32 first = 0;
        u32 count = 0;
        bool filled = false;
    };

    Array<Slot> slots_;
    Array<CollisionProxy> proxies_;
    u32 window_;
    u64 recorded_ = 0;
    mutable u64 refusals_ = 0;
};

}  // namespace cy::net
