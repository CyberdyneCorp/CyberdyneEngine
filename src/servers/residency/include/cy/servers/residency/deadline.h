#pragma once
// Deadline propagation: one prediction, many preparations. Task 4.1.
//
// `residency` — "Deadline propagation". When a subsystem predicts a future need — the world
// predicting arrival at a region, a sequence declaring an upcoming shot or camera cut, a teleport
// being initiated — that prediction is expressed **once** and propagated to every consumer:
// geometry pages, texture pages, shadow warm-up, illumination prefetch, audio preload and world
// cell preparation. "Subsystems SHALL NOT each derive the same prediction independently, since
// independent derivations disagree about how soon."
//
// That last clause is the design. `announce()` takes one number and writes one deadline per
// consumer in the mask, all carrying the same due time and the same confidence. There is no
// per-subsystem prediction API, so there is nowhere for a second derivation to live.
//
// --- A DEADLINE IS A SCHEDULING INPUT AND NOTHING ELSE
// --------------------------------------------
//
// "A deadline SHALL be a scheduling input, consistent with the task system's rule that deadlines
// influence when work runs and never what the simulation computes." Nothing here returns a value a
// simulation could read: a deadline reaches `RequestInputs::seconds_until_needed` and stops. A
// missed deadline is *reported*, not corrected — the frame renders coarser, which is the whole
// point of a guaranteed coarse representation.
//
// --- WHY ACCURACY IS COUNTED HERE
// ------------------------------------------------------------------
//
// "Prediction quality is measurable": the fraction of prefetched pages actually sampled must be
// reported so that prediction can be tuned against evidence. The counters sit beside the
// predictions rather than in each subsystem because the question is about the *predictor*, and a
// per-subsystem counter would answer "how good is texture prefetching" when the honest question is
// "how good is the camera extrapolator that texture prefetching believed".

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/servers/residency/types.h>

namespace cy::residency {

/// Who predicted. `residency` names the strongest of these explicitly: "A **compiled sequence** is
/// the strongest predictor available to the engine" — it knows exactly where its camera will be,
/// unlike extrapolation, which guesses the next few hundred milliseconds.
enum class PredictionSource : u8 {
    CameraMotion = 0,  // extrapolation: short horizon, moderate confidence
    WorldCell,         // the world predicting arrival at a region
    Sequence,          // a compiled sequence's shot list; the strongest predictor
    Teleport,          // a destination announced before the jump
    Network,           // a peer's position, or a server's preload instruction
    Count,
};

[[nodiscard]] const char* prediction_source_name(PredictionSource source) noexcept;

/// The prediction, as it is announced. One of these becomes up to `kSubsystemCount` deadlines.
struct Prediction {
    PredictionSource source = PredictionSource::CameraMotion;
    /// What is being predicted about: a world cell, a shot, a teleport destination. Opaque, for the
    /// same reason a `PageKey`'s page number is.
    u64 region = 0;
    /// How far ahead, in seconds. The one number every consumer will schedule against.
    f64 seconds_until = 0.0;
    f32 confidence = 1.0F;
    /// Which consumers must prepare. `kAllSubsystems` for an arrival; a narrower mask for a
    /// prediction that genuinely concerns fewer — a sequence's audio-only pre-roll, say.
    SubsystemMask consumers = kAllSubsystems;
};

/// One consumer's share of a prediction.
struct Deadline {
    Subsystem subsystem = Subsystem::Geometry;
    u64 region = 0;
    /// Absolute, on whatever clock was passed to `announce`. Absolute rather than relative because
    /// a deadline outlives the frame that announced it and a relative one would have to be
    /// decremented by somebody.
    f64 due_seconds = 0.0;
    f32 confidence = 1.0F;
    PredictionSource source = PredictionSource::CameraMotion;
    bool satisfied = false;
    bool missed = false;
};

/// What prefetching bought. `residency`: the fraction of prefetched pages actually sampled.
struct PredictionAccuracy {
    u64 prefetched = 0;
    u64 sampled = 0;
    u64 deadlines_set = 0;
    u64 deadlines_met = 0;
    u64 deadlines_missed = 0;

    /// Sampled over prefetched, in [0, 1]. One when nothing was prefetched — a predictor that made
    /// no claim has not made a wrong one.
    [[nodiscard]] f64 sample_rate() const noexcept {
        return (prefetched == 0) ? 1.0 : static_cast<f64>(sampled) / static_cast<f64>(prefetched);
    }
    [[nodiscard]] f64 hit_rate() const noexcept {
        return (deadlines_set == 0)
                   ? 1.0
                   : static_cast<f64>(deadlines_met) / static_cast<f64>(deadlines_set);
    }
};

/// The one place a prediction becomes deadlines.
class DeadlineBus {
public:
    explicit DeadlineBus(Allocator& allocator = current_allocator()) noexcept
        : deadlines_(allocator), slots_(allocator) {}

    DeadlineBus(const DeadlineBus&) = delete;
    DeadlineBus& operator=(const DeadlineBus&) = delete;
    DeadlineBus(DeadlineBus&&) noexcept = default;
    DeadlineBus& operator=(DeadlineBus&&) noexcept = default;
    ~DeadlineBus() = default;

    /// Announce once. Writes one deadline per consumer in `prediction.consumers`, all due at
    /// `now + prediction.seconds_until`. Re-announcing the same region for the same consumer moves
    /// the deadline *earlier* only — a prediction that pushes a deadline back is a prediction that
    /// has changed its mind, and the safe reading of two disagreeing predictions is the urgent one.
    Status announce(const Prediction& prediction, f64 now) noexcept;

    /// A consumer reports it has prepared the region. Met if it is not yet due, missed if it is.
    bool satisfy(Subsystem subsystem, u64 region, f64 now) noexcept;

    /// Move the clock forward: anything due and unsatisfied becomes a miss, once. Returns how many
    /// newly missed.
    u32 advance(f64 now) noexcept;

    /// Seconds until due for one consumer's region, or `kNoDeadline` when it has none. This is the
    /// number that reaches `RequestInputs::seconds_until_needed`, and it is the whole public
    /// surface a request-building subsystem needs.
    [[nodiscard]] f64 seconds_until(Subsystem subsystem, u64 region, f64 now) const noexcept;

    [[nodiscard]] const Deadline* find(Subsystem subsystem, u64 region) const noexcept;

    /// Copy out the outstanding deadlines, in announcement order. Returns how many were written.
    u32 collect(Deadline* out, u32 capacity) const noexcept;

    [[nodiscard]] usize size() const noexcept { return deadlines_.size(); }
    [[nodiscard]] u32 outstanding(Subsystem subsystem) const noexcept;
    /// Deadlines this consumer was given and did not meet. Counted here rather than derived by the
    /// caller, because a miss is discovered inside `advance()` and `satisfy()` and nowhere else.
    [[nodiscard]] u64 misses(Subsystem subsystem) const noexcept;

    /// Prefetch accounting, per prediction source. `sampled` is reported by whoever samples.
    void record_prefetched(PredictionSource source, u64 pages) noexcept;
    void record_sampled(PredictionSource source, u64 pages) noexcept;
    [[nodiscard]] PredictionAccuracy accuracy(PredictionSource source) const noexcept;
    [[nodiscard]] PredictionAccuracy accuracy() const noexcept;  // every source, summed

    /// Forget the satisfied and missed deadlines, keeping the outstanding ones and the counters.
    /// What a frame calls after reading its diagnostics.
    void retire_resolved() noexcept;

    void clear() noexcept;

private:
    [[nodiscard]] static u64 slot_key(Subsystem subsystem, u64 region) noexcept {
        return (static_cast<u64>(subsystem) << 56U) | (region & PageKey::kPageMask);
    }
    void rebuild_slots() noexcept;

    Array<Deadline> deadlines_;
    HashMap<u64, usize> slots_;
    PredictionAccuracy accuracy_[static_cast<u32>(PredictionSource::Count)] = {};
    u64 misses_[kSubsystemCount] = {};
};

}  // namespace cy::residency
