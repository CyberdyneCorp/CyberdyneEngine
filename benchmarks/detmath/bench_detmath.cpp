// The deterministic math kernel's costs, one dependent chain per operation, so each figure is the
// LATENCY a simulation step pays when the next operation needs this one's result. Design §11 states
// the budgets on the x86-64 reference runner:
//
//     *         <= 2x an f64 multiply in a dependent chain   (detmath/mul against detmath/f64-mul)
//     /         <= 30 ns
//     sqrt      <= 40 ns
//     sin, cos  <= 25 ns
//     atan2     <= 50 ns
//     exp2, log2 <= 30 ns each
//
// The committed baselines in benchmarks/baseline.json are what defends them from here on: a change
// that makes one of these slower by more than its tolerance fails `just test-bench`.

#include <cy/bench/bench.h>
#include <cy/core/detmath/fixed.h>
#include <cy/core/detmath/functions.h>

#include <cstdint>

namespace {

using cy::detmath::Angle;
using cy::detmath::Fixed;
namespace dm = cy::detmath;

}  // namespace

CY_BENCHMARK(
    "detmath/f64-mul",
    "An f64 multiply in a dependent chain: the yardstick design §11 measures the Fixed "
    "multiply against. Not engine code; a regression here is the machine or the harness.") {
    double value = 1.0000001;
    const double factor = 0.99999993;
    for (std::uint64_t i = 0; i < CY_BENCH_ITERATIONS; ++i) {
        value *= factor;
    }
    CY_BENCH_KEEP(value);
}

CY_BENCHMARK("detmath/mul",
             "Fixed * Fixed in a dependent chain: the 64x64->128 product, the rounding add and the "
             "shift. A regression means the 128-bit multiply path stopped compiling to one "
             "instruction pair, and every authoritative position update pays it.") {
    Fixed value = Fixed::from_raw(0x1'2345'6789);
    const Fixed factor = Fixed::from_raw(0xFFFF'FF00);
    for (std::uint64_t i = 0; i < CY_BENCH_ITERATIONS; ++i) {
        value = value * factor;
    }
    CY_BENCH_KEEP(value.raw);
}

CY_BENCHMARK("detmath/div",
             "Fixed / Fixed in a dependent chain: a 128/64 division. Budget 30 ns. A regression is "
             "felt by every normalisation and every ratio gameplay computes per tick.") {
    Fixed value = Fixed::from_int(1000);
    const Fixed divisor = Fixed::from_raw(0x1'0000'0100);
    const Fixed offset = Fixed::from_int(3);
    for (std::uint64_t i = 0; i < CY_BENCH_ITERATIONS; ++i) {
        value = value / divisor + offset;
    }
    CY_BENCH_KEEP(value.raw);
}

CY_BENCHMARK("detmath/sqrt",
             "The correctly rounded square root of a Fixed, in a dependent chain. Budget 40 ns. A "
             "regression means the seeded Newton step started missing and the exact correction "
             "loop is doing the work; every distance and normalisation pays it.") {
    Fixed value = Fixed::from_int(12345);
    const Fixed offset = Fixed::from_int(777);
    for (std::uint64_t i = 0; i < CY_BENCH_ITERATIONS; ++i) {
        value = dm::sqrt(value + offset) * offset;
    }
    CY_BENCH_KEEP(value.raw);
}

CY_BENCHMARK("detmath/sin",
             "sin of a binary angle, its result fed back into the next angle. Budget 25 ns for one "
             "octant reduction and a Horner evaluation in Q2.62. Headings and every rotation an "
             "authoritative unit makes pay it.") {
    Angle angle = Angle::from_raw(0x1234'5678U);
    for (std::uint64_t i = 0; i < CY_BENCH_ITERATIONS; ++i) {
        angle = Angle::from_raw(angle.raw + 0x9E37'79B9U +
                                static_cast<std::uint32_t>(dm::sin(angle).raw));
    }
    CY_BENCH_KEEP(angle.raw);
}

CY_BENCHMARK("detmath/atan2",
             "atan2 of two Fixed values, its result fed back into the next pair. Budget 50 ns: one "
             "ratio division, the second reduction's division, and a Horner evaluation. Steering "
             "toward a target pays it per unit per tick.") {
    Fixed y = Fixed::from_int(3);
    const Fixed x = Fixed::from_int(-4);
    for (std::uint64_t i = 0; i < CY_BENCH_ITERATIONS; ++i) {
        y = y + Fixed::from_raw(static_cast<std::int64_t>(dm::atan2(y, x).raw) - 0x7FFF'FFFF);
    }
    CY_BENCH_KEEP(y.raw);
}

CY_BENCHMARK(
    "detmath/exp2",
    "exp2 of a Fixed in [-4, 4), its result fed back. Budget 30 ns: a shift for the integer "
    "part and a degree-9 Horner evaluation for the fraction.") {
    Fixed value = Fixed::from_raw(0x1'8000'0000);
    const Fixed mask = Fixed::from_raw((std::int64_t{1} << 35) - 1);
    const Fixed bias = Fixed::from_int(4);
    for (std::uint64_t i = 0; i < CY_BENCH_ITERATIONS; ++i) {
        value = Fixed::from_raw((dm::exp2(value).raw & mask.raw)) - bias;
    }
    CY_BENCH_KEEP(value.raw);
}

CY_BENCHMARK("detmath/log2",
             "log2 of a positive Fixed, its result fed back. Budget 30 ns: countl_zero, one exact "
             "comparison, one division and an odd series.") {
    Fixed value = Fixed::from_int(1000);
    const Fixed offset = Fixed::from_int(100);
    for (std::uint64_t i = 0; i < CY_BENCH_ITERATIONS; ++i) {
        value = dm::log2(value) + offset;
    }
    CY_BENCH_KEEP(value.raw);
}
