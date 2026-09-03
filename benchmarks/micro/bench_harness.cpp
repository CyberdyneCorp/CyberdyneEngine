// The trivial benchmark that exercises the harness end to end: registration, iteration scaling,
// the calibration ratio, the results file, and the threshold compare.py checks against it.

#include <cy/bench/bench.h>

#include <cstdint>

CY_BENCHMARK("harness/xorshift64-star",
             "One xorshift64* step: three shift-xors and a multiply, in a dependent chain. It "
             "measures the timing loop itself against the calibration workload, so a regression "
             "here is a regression in the harness — an iteration count that stopped scaling, a "
             "calibration that stopped being latency-bound, or a compiler that started deleting "
             "the body — and not in any engine code.") {
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    for (std::uint64_t i = 0; i < CY_BENCH_ITERATIONS; ++i) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        state *= 0x2545f4914f6cdd1dULL;
    }
    CY_BENCH_KEEP(state);
}
