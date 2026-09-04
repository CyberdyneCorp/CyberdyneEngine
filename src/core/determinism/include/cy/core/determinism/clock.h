#pragma once
// The simulation clock: an exact rational tick rate, an accumulator that cannot drift, and the
// (epoch, tick) that is the authoritative unit of simulation time. Task 4.2.1.
//
// `simulation-and-determinism` — "The simulation clock": ticks are fixed, the tick number is the
// authoritative unit of simulation time, wall-clock time is never authoritative, the rate is an
// exact rational "so that accumulating a rounded step never drifts", a frame may execute zero, one
// or several ticks under a configured bound, and **the step is never lengthened to catch up**.
//
// --- THE ONE STRUCTURAL DECISION IN THIS FILE ----------------------------------------------------
//
// THIS CLASS CANNOT READ A CLOCK. It has no member that calls one, includes no header that offers
// one, and its only source of elapsed time is `accumulate()`, which the host calls with a duration
// it measured. That is the whole of "every system reads simulation time from the clock, never a
// wall clock" made structural rather than conventional: a system handed a `const SimulationClock&`
// has no wall clock reachable through it, and the runtime hands systems nothing else.
//
// The honest boundary: it stops a system from reading a wall clock *through the clock*. A system
// that calls the platform's monotonic counter directly is still only catchable by the determinism
// lint, which is M9's. Said again, in the same words, in classification.h — it is the same trap.
//
// --- WHY THE ACCUMULATOR IS SCALED ---------------------------------------------------------------
//
// 1/60 s is 16 666 666.66… ns, so a step expressed in nanoseconds is wrong by a third of a
// nanosecond every tick — 1.2 ms per hour, and the requirement's "no drift" scenario is a session
// that runs for hours. The accumulator therefore holds *nanoseconds multiplied by the rate's
// numerator*, and one step is exactly `denominator * 1e9` of those units. Subtracting a step is
// then an exact integer subtraction and the residue is exact, so the interpolation alpha is exact
// too. Nothing rounds anywhere on this path.
//
// Range: the accumulator never holds more than a frame's worth plus one step, so with a numerator
// bounded by kMaxTicksPerSecond the product stays four orders of magnitude below i64's range. The
// bound is checked when a rate is validated rather than assumed.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>

namespace cy::determinism {

inline constexpr i64 kNanosecondsPerSecond = 1'000'000'000;

/// The upper bound on the *rate* — numerator divided by denominator. Ten thousand ticks per second
/// is far past anything a simulation would ask for.
inline constexpr u32 kMaxTicksPerSecond = 10'000;

/// The upper bound on either term of the rational.
///
/// It is separate from the rate bound because a legitimate rate can have large terms: 30000/1001 is
/// 29.97 ticks per second, the rate that exists precisely because it cannot be written as a step.
/// What the term bound protects is the scaled accumulator's range — it holds nanoseconds times the
/// numerator, and one step is `denominator * 1e9` of those units, so with both terms below a
/// million the arithmetic stays three orders of magnitude inside i64.
inline constexpr u32 kMaxRateTerm = 1'000'000;

/// Ticks per second, as an exact rational. `120/1` and `30000/1001` are both spellable; the second
/// is the one that makes the rational form worth having.
struct TickRate {
    u32 numerator = 60;
    u32 denominator = 1;

    friend constexpr bool operator==(TickRate, TickRate) noexcept = default;
};

/// Reject a rate that cannot be simulated: a zero on either side, or a numerator past the bound the
/// accumulator's arithmetic is proved in range for.
[[nodiscard]] Status validate(TickRate rate) noexcept;

/// The step, in nanoseconds, **rounded**. For reporting and for a subsystem that needs a duration
/// and not a moment — never for advancing the clock, which uses the exact form.
[[nodiscard]] constexpr Nanoseconds step_nanoseconds(TickRate rate) noexcept {
    if (rate.numerator == 0) {
        return 0;
    }
    return (static_cast<i64>(rate.denominator) * kNanosecondsPerSecond) /
           static_cast<i64>(rate.numerator);
}

/// The step in seconds, as the f32 a gameplay system integrates with. Exact for a rate whose
/// reciprocal is representable and correctly rounded otherwise — and it is a *derived* value, so
/// accumulating it is not how the clock advances.
[[nodiscard]] constexpr f32 step_seconds(TickRate rate) noexcept {
    if (rate.numerator == 0) {
        return 0.0F;
    }
    return static_cast<f32>(static_cast<f64>(rate.denominator) / static_cast<f64>(rate.numerator));
}

/// How a frame decides how many ticks to run.
enum class TickMode : u8 {
    /// Ticks are driven by measured elapsed time, bounded by `max_ticks_per_frame`. What a game
    /// runs.
    Realtime = 0,
    /// Exactly `fixed_ticks_per_frame` ticks per frame, whatever the wall clock says.
    /// `engine-architecture`'s `--fixed-step <n>`: reproducible simulation for recording and
    /// automated tests, and the mode the M2 artefact runs in.
    FixedStep = 1,
};

struct ClockConfig {
    TickRate rate;
    /// `engine-architecture`'s default and cap: exceeding it discards the excess so the loop cannot
    /// enter a death spiral. Discarding is the only permitted response — lengthening the step is on
    /// `simulation-and-determinism`'s forbidden list.
    u32 max_ticks_per_frame = 8;
    TickMode mode = TickMode::Realtime;
    /// FixedStep only. One is the ordinary choice; a headless batch run may want more.
    u32 fixed_ticks_per_frame = 1;
    /// Where the timeline starts. Non-zero when a session is resumed from a save or a replay.
    Epoch epoch;
    u64 tick = 0;
};

/// What a frame's worth of accumulation produced. Returned rather than read back field by field so
/// that the caller cannot use half of it.
struct FrameTicks {
    u32 ticks = 0;
    /// The accumulator's residue as a fraction of one step, in [0, 1). `engine-architecture`'s
    /// interpolation alpha. Exact: it is a ratio of two integers the clock never rounded.
    f32 alpha = 0.0F;
    /// Nanoseconds thrown away because the cap was reached. Non-zero means frames were slower than
    /// the cap allows and the excess was dropped rather than chased.
    u64 discarded_ns = 0;
};

/// Fixed ticks, epochs, and nothing that can tell you what time it is.
class SimulationClock {
public:
    SimulationClock() = default;

    /// Refuses an invalid rate rather than clamping it: a simulation running at a rate nobody asked
    /// for is worse than one that did not start.
    [[nodiscard]] Status configure(const ClockConfig& config) noexcept;

    [[nodiscard]] TickRate rate() const noexcept { return config_.rate; }
    [[nodiscard]] TickMode mode() const noexcept { return config_.mode; }
    [[nodiscard]] u32 max_ticks_per_frame() const noexcept { return config_.max_ticks_per_frame; }

    [[nodiscard]] u64 tick() const noexcept { return tick_; }
    [[nodiscard]] Epoch epoch() const noexcept { return epochs_.current(); }
    [[nodiscard]] SimulationPoint now() const noexcept { return {epochs_.current(), tick_}; }
    [[nodiscard]] const EpochCounter& epochs() const noexcept { return epochs_; }

    /// Simulation seconds since tick zero of this epoch, computed from the tick and the rational
    /// rate. Derived on demand and never accumulated, which is why it cannot drift.
    [[nodiscard]] f64 seconds() const noexcept { return seconds_at(tick_); }
    [[nodiscard]] f64 seconds_at(u64 tick) const noexcept;

    /// The step a system integrates with. Constant for the life of the session by construction:
    /// `configure()` is the only thing that sets the rate, and a running clock refuses it.
    [[nodiscard]] f32 delta_seconds() const noexcept { return step_seconds(config_.rate); }

    /// Feed the frame's measured elapsed time and learn how many ticks it bought.
    ///
    /// In `FixedStep` mode the argument is ignored and the answer is always
    /// `fixed_ticks_per_frame` with an alpha of zero — that is what makes a recorded run
    /// reproducible on a machine of a different speed.
    [[nodiscard]] FrameTicks accumulate(Nanoseconds elapsed_ns) noexcept;

    /// Consume one of the ticks `accumulate()` reported. Advances the tick number and the
    /// accumulator by exactly one step.
    ///
    /// Separate from `accumulate()` because the tick number has to be correct *while the tick
    /// runs*: a system asking the clock what tick it is during the second of three catch-up ticks
    /// must be told the second, not the third.
    void advance() noexcept;

    /// Leave the current epoch. The tick continues where the caller puts it — a checkpoint restore
    /// rewinds it, a hot reload does not — so the tick is an explicit argument rather than a
    /// silently preserved field.
    Epoch reset(EpochReason reason, u64 tick) noexcept;

    /// Resume a timeline recorded elsewhere. Clears the accumulator: a residue measured against
    /// another session's frame boundaries means nothing here.
    void resume(SimulationPoint point, EpochReason reason) noexcept;

    /// The alpha as it stands, without accumulating. What the render half of a frame reads.
    [[nodiscard]] f32 interpolation_alpha() const noexcept;

    /// Total time discarded at the cap, over the session. A rising number is the diagnostic
    /// counter `engine-architecture`'s "slow frame is bounded" scenario requires.
    [[nodiscard]] u64 discarded_ns() const noexcept { return discarded_ns_; }
    /// How many frames hit the cap. Distinct from the nanoseconds, because one 400 ms frame and a
    /// hundred marginal ones are different problems.
    [[nodiscard]] u64 capped_frames() const noexcept { return capped_frames_; }

private:
    /// One step, in accumulator units: `denominator * 1e9`. See the header comment.
    [[nodiscard]] i64 step_scaled() const noexcept { return denominator_ * kNanosecondsPerSecond; }

    ClockConfig config_;
    /// The rate's two halves widened once, because every arithmetic use of them is in i64 and
    /// converting at each use reads as though the width were in doubt.
    i64 numerator_ = 60;
    i64 denominator_ = 1;
    EpochCounter epochs_;
    u64 tick_ = 0;
    /// Nanoseconds times the rate's numerator. Never negative: `accumulate()` refuses a negative
    /// elapsed time, because a clock that went backwards is a platform defect and pretending
    /// otherwise would let it steal ticks.
    i64 accumulator_ = 0;
    u64 discarded_ns_ = 0;
    u64 capped_frames_ = 0;
};

}  // namespace cy::determinism
