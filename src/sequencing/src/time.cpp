// Exact time: the three functions time.h declares and the overflow argument they rest on.

#include <cy/sequencing/time.h>

#include <cmath>

namespace cy::sequencing {
namespace {

/// Floored division. `i64`'s own `/` truncates toward zero, which would make the residual of a
/// reverse advance negative and the next advance consume it twice.
[[nodiscard]] i64 floor_div(i64 numerator, i64 denominator) noexcept {
    const i64 quotient = numerator / denominator;
    const i64 remainder = numerator % denominator;
    return (remainder != 0 && ((remainder < 0) != (denominator < 0))) ? quotient - 1 : quotient;
}

}  // namespace

const char* clock_domain_name(ClockDomain domain) noexcept {
    switch (domain) {
        case ClockDomain::Presentation:
            return "Presentation";
        case ClockDomain::Simulation:
            return "Simulation";
        case ClockDomain::Cinematic:
            return "Cinematic";
        case ClockDomain::RealTime:
            return "RealTime";
        case ClockDomain::External:
            return "External";
        case ClockDomain::Count:
            break;
    }
    return "Unknown";
}

bool domain_permits_authoritative(ClockDomain domain) noexcept {
    // The specification's table, transcribed once: only `Simulation` carries authoritative
    // gameplay, because only it is related to the simulation tick by a declared mapping.
    return domain == ClockDomain::Simulation;
}

Expected<SequenceTime, Error> TimeAccumulator::advance(Rate rate, PlayRate speed,
                                                       i64 delta_nanoseconds) noexcept {
    if (!rate.valid()) {
        return fail(ErrorCode::InvalidArgument, "sequence rate out of range");
    }
    if (!speed.valid()) {
        return fail(ErrorCode::InvalidArgument, "play rate out of range");
    }
    if (delta_nanoseconds < 0 || delta_nanoseconds > kMaxAdvanceNanoseconds) {
        return fail(ErrorCode::InvalidArgument,
                    "a single advance is at most one second of wall time");
    }

    // ticks = ns * rate.numerator * speed.numerator / (rate.denominator * speed.denominator * 10^6)
    //
    // The 10^6 is 10^9 nanoseconds per second divided by the 10^3 ticks per frame — the
    // cancellation `kTicksPerFrame`'s comment promises. The worst case of each intermediate, with
    // the bounds `Rate::valid()` and `PlayRate::valid()` enforce:
    //
    //   numerator   1e9 ns * 240000 * 10000            = 2.4e18   < 9.22e18
    //   denominator 100000 * 10000 * 1e6                = 1e15
    //   ticks * denominator <= numerator, by construction.
    const i64 numerator =
        delta_nanoseconds * static_cast<i64>(rate.numerator) * static_cast<i64>(speed.numerator);
    const i64 denominator =
        static_cast<i64>(rate.denominator) * static_cast<i64>(speed.denominator) * 1000000;

    residual_ += numerator;
    const i64 ticks = floor_div(residual_, denominator);
    residual_ -= ticks * denominator;
    return SequenceTime::from_ticks(ticks);
}

i64 nanoseconds_from_seconds(f64 seconds) noexcept {
    return static_cast<i64>(std::llround(seconds * static_cast<f64>(kNanosecondsPerSecond)));
}

f64 seconds_from_time(Rate rate, SequenceTime time) noexcept {
    if (!rate.valid()) {
        return 0.0;
    }
    const f64 frames = static_cast<f64>(time.ticks()) / static_cast<f64>(kTicksPerFrame);
    return frames * static_cast<f64>(rate.denominator) / static_cast<f64>(rate.numerator);
}

}  // namespace cy::sequencing
