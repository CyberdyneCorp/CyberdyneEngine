#pragma once
// Exact sequence time: a frame and a subframe at a declared rational rate. M8.c task 3.2.
//
// `sequencing-and-cinematics` — "Exact time": sequence time "SHALL be represented **exactly** — a
// frame and subframe at a declared rational rate — and SHALL NOT be an accumulated floating-point
// value", "the same time reached by playing, seeking, stepping, or replaying SHALL be the same
// instant", and "repeated loops SHALL NOT drift".
//
// ================================================================================================
// WHY AN INTEGER AND NOT A FLOAT, SPELLED OUT ONCE SO NOBODY RE-ADDS THE FLOAT
// ================================================================================================
//
// A cinematic that loops for hours accumulates one rounding error per advance. At 60 Hz over four
// hours that is 864,000 additions; in `f32` the accumulated instant is wrong by more than a frame
// long before the end, and — worse than wrong — the value reached by PLAYING to a time differs from
// the value reached by SEEKING to it, which is the requirement's second sentence. So the instant is
// an integer count of subframe ticks and the only arithmetic performed on it is integer addition.
//
// The wall clock is a float at the boundary and nowhere inside: `TimeAccumulator::advance()` takes
// **nanoseconds as an integer** and keeps the sub-tick remainder in exact integer form, so the
// residue of one advance is carried into the next rather than being rounded away. That is what
// makes a thousand 16.666 ms advances land on exactly the instant a single 16.666 s advance does.
//
// ================================================================================================
// WHY THE PLAY RATE IS A FRACTION
// ================================================================================================
//
// "Set play rate" is playback control's, and a play rate of 0.1 in `f32` is 0.100000001490116...,
// which reintroduces the drift the tick count removes — at one tenth speed the error is ten times
// as visible. `PlayRate` is therefore a signed rational, its negative form is reverse playback, and
// half speed is exactly one half.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

namespace cy::sequencing {

/// Subframe ticks per frame. A thousand: it divides the subdivisions an editor scrubber offers
/// (halves, quarters, fifths, eighths within a rounding of a tick), it keeps a frame's worth of
/// blend weight readable as a per-mille, and — the reason it is exactly a thousand and not 1024 —
/// it cancels against a nanosecond so that the tick conversion below multiplies by 10^6 rather than
/// by 10^9, which is what keeps every intermediate inside an `i64`.
inline constexpr i64 kTicksPerFrame = 1000;

inline constexpr i64 kNanosecondsPerSecond = 1000000000;

/// The largest single advance. A caller with a longer delta — a debugger breakpoint, a level load —
/// calls twice; the alternative is an intermediate that overflows, silently, on a frame nobody will
/// ever reproduce.
inline constexpr i64 kMaxAdvanceNanoseconds = kNanosecondsPerSecond;

/// A sequence's frame rate, exactly. 24, 30, 60 — and 30000/1001, which is what "29.97" is.
struct Rate {
    u32 numerator = 24;
    u32 denominator = 1;

    /// The bounds exist so that the tick conversion cannot overflow an `i64`; see time.cpp, where
    /// the arithmetic that depends on them is written out with its worst case.
    [[nodiscard]] bool valid() const noexcept {
        return numerator >= 1 && numerator <= 240000 && denominator >= 1 && denominator <= 100000;
    }

    [[nodiscard]] bool operator==(const Rate& other) const noexcept {
        return numerator == other.numerator && denominator == other.denominator;
    }
};

/// Playback speed as a signed rational. Negative is reverse playback; zero is a paused clock that
/// still ticks its subsystems, which is not the same thing as `Paused` and is why zero is legal.
struct PlayRate {
    i32 numerator = 1;
    u32 denominator = 1;

    [[nodiscard]] bool valid() const noexcept {
        return denominator >= 1 && denominator <= 10000 && numerator >= -10000 &&
               numerator <= 10000;
    }
    [[nodiscard]] bool reverse() const noexcept { return numerator < 0; }
    [[nodiscard]] static PlayRate normal() noexcept { return PlayRate{1, 1}; }
    [[nodiscard]] static PlayRate half() noexcept { return PlayRate{1, 2}; }
};

/// An instant on a sequence's own timeline.
///
/// A VALUE with no rate inside it. The rate belongs to the sequence, and putting it here would
/// invite two instants at different rates to be compared, which is a question with no answer.
class SequenceTime {
public:
    constexpr SequenceTime() noexcept = default;

    [[nodiscard]] static constexpr SequenceTime from_ticks(i64 ticks) noexcept {
        return SequenceTime{ticks};
    }
    /// `subframe` may be any value, including negative and beyond a frame: it is added, not
    /// validated, because "frame 3 minus 200 ticks" is a legitimate way to spell a pre-roll.
    [[nodiscard]] static constexpr SequenceTime from_frame(i64 frame, i64 subframe = 0) noexcept {
        return SequenceTime{(frame * kTicksPerFrame) + subframe};
    }

    [[nodiscard]] constexpr i64 ticks() const noexcept { return ticks_; }
    /// Floored, so frame(-1 tick) is -1 rather than 0. A truncating division would make the frame
    /// before zero and the frame after zero both "frame 0", which is a defect a pre-roll finds.
    [[nodiscard]] constexpr i64 frame() const noexcept {
        return (ticks_ >= 0) ? (ticks_ / kTicksPerFrame)
                             : -((-ticks_ + kTicksPerFrame - 1) / kTicksPerFrame);
    }
    [[nodiscard]] constexpr i64 subframe() const noexcept {
        return ticks_ - (frame() * kTicksPerFrame);
    }

    constexpr SequenceTime& operator+=(SequenceTime other) noexcept {
        ticks_ += other.ticks_;
        return *this;
    }
    constexpr SequenceTime& operator-=(SequenceTime other) noexcept {
        ticks_ -= other.ticks_;
        return *this;
    }

    [[nodiscard]] friend constexpr SequenceTime operator+(SequenceTime a, SequenceTime b) noexcept {
        return SequenceTime{a.ticks_ + b.ticks_};
    }
    [[nodiscard]] friend constexpr SequenceTime operator-(SequenceTime a, SequenceTime b) noexcept {
        return SequenceTime{a.ticks_ - b.ticks_};
    }
    [[nodiscard]] friend constexpr bool operator==(SequenceTime a, SequenceTime b) noexcept {
        return a.ticks_ == b.ticks_;
    }
    [[nodiscard]] friend constexpr bool operator!=(SequenceTime a, SequenceTime b) noexcept {
        return a.ticks_ != b.ticks_;
    }
    [[nodiscard]] friend constexpr bool operator<(SequenceTime a, SequenceTime b) noexcept {
        return a.ticks_ < b.ticks_;
    }
    [[nodiscard]] friend constexpr bool operator<=(SequenceTime a, SequenceTime b) noexcept {
        return a.ticks_ <= b.ticks_;
    }
    [[nodiscard]] friend constexpr bool operator>(SequenceTime a, SequenceTime b) noexcept {
        return a.ticks_ > b.ticks_;
    }
    [[nodiscard]] friend constexpr bool operator>=(SequenceTime a, SequenceTime b) noexcept {
        return a.ticks_ >= b.ticks_;
    }

private:
    explicit constexpr SequenceTime(i64 ticks) noexcept : ticks_(ticks) {}

    i64 ticks_ = 0;
};

/// Which clock a sequence runs on. `sequencing-and-cinematics` — "Clock domains", and the table
/// there is the reason `Simulation` is singled out everywhere below: it is the only domain a
/// sequence may carry authoritative gameplay on.
enum class ClockDomain : u8 {
    Presentation = 0,
    Simulation,
    Cinematic,
    RealTime,
    External,
    Count,
};

[[nodiscard]] const char* clock_domain_name(ClockDomain domain) noexcept;

/// Whether this domain may carry authoritative gameplay. One function, consulted by the compiler
/// and by nothing else, so the table in the specification has exactly one transcription.
[[nodiscard]] bool domain_permits_authoritative(ClockDomain domain) noexcept;

/// Wall time to sequence ticks, with the sub-tick remainder carried rather than rounded.
///
/// The whole of the exactness claim lives here: `advance()` performs one integer multiply, one
/// floored division and one subtraction, and the remainder that was not consumed stays in
/// `residual()` for the next call. Nothing rounds, so a million advances of one frame's nanoseconds
/// land on exactly the millionth frame.
class TimeAccumulator {
public:
    /// The ticks `delta_nanoseconds` is worth at `rate` and `speed`, with the remainder retained.
    /// Fails on an invalid rate or a delta beyond `kMaxAdvanceNanoseconds` rather than overflowing.
    [[nodiscard]] Expected<SequenceTime, Error> advance(Rate rate, PlayRate speed,
                                                        i64 delta_nanoseconds) noexcept;

    /// Drop the retained remainder. What a seek does: an instant reached by seeking must not carry
    /// the fraction of a tick left over from playing toward a different one.
    void reset() noexcept { residual_ = 0; }

    [[nodiscard]] i64 residual() const noexcept { return residual_; }

private:
    /// In units of (rate.denominator * speed.denominator * 10^6) per tick — see time.cpp.
    i64 residual_ = 0;
};

/// Seconds as a `f64` to nanoseconds, rounded once, at the boundary where a caller's clock is a
/// float. Provided so that every caller rounds the same way rather than each writing its own cast.
[[nodiscard]] i64 nanoseconds_from_seconds(f64 seconds) noexcept;

/// The instant `time` falls on, in seconds, for a diagnostic or a display. Lossy on purpose and
/// named so: nothing in evaluation calls it.
[[nodiscard]] f64 seconds_from_time(Rate rate, SequenceTime time) noexcept;

}  // namespace cy::sequencing
