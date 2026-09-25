// cy/bench/bench.h — declaring a benchmark, and what a benchmark must declare.
//
// Task 4.1.5. `testing-and-quality` asks for two things this header enforces rather than documents.
//
// The first: "each benchmark SHALL declare what it measures and what a regression would mean, so a
// failure is actionable rather than mysterious". The description is a required argument, printed in
// the table and carried into the results file, so the person reading a failed threshold in CI reads
// the author's sentence rather than a name.
//
// The second: a threshold that is checked. The runner measures; benchmarks/tools/compare.py
// decides. A benchmark with no baseline entry fails the comparison rather than being reported as a
// number, because a number with no budget is a measurement, not a gate.

#ifndef CY_BENCH_BENCH_H
#define CY_BENCH_BENCH_H

#include <cstdint>

namespace cy::bench {

/// One iteration count in, one benchmark body run. The body owns its own loop — the alternative,
/// calling the body once per iteration, measures the call as much as the work at the scale a
/// microbenchmark operates on.
using Body = void (*)(std::uint64_t iterations);

/// The iteration count the runner tries first before scaling towards the minimum time. Right for a
/// body whose one iteration is nanoseconds; a body whose one iteration is milliseconds declares a
/// smaller one with CY_BENCHMARK_STARTING_AT, or it pays a thousand of them per sample.
inline constexpr std::uint64_t kDefaultFirstIterations = 1024;

/// Registers a benchmark at static initialisation. Constructed by CY_BENCHMARK; there is no other
/// way to add one, so every benchmark in a binary carries a description.
struct Registration {
    Registration(const char* name, const char* description, Body body,
                 std::uint64_t first_iterations = kDefaultFirstIterations);
};

/// Keep a value the optimiser would otherwise delete. A benchmark whose result is unused measures
/// nothing, and measures it very quickly.
template <typename T>
inline void keep(const T& value) {
#if defined(_MSC_VER) && !defined(__clang__)
    // MSVC has no inline asm on x64. A volatile read of the object is enough to make the
    // computation observable.
    const volatile T sink = value;
    (void)sink;
#else
    asm volatile("" : : "r,m"(value) : "memory");
#endif
}

}  // namespace cy::bench

#define CY_BENCH_CONCAT_IMPL(a, b) a##b
#define CY_BENCH_CONCAT(a, b) CY_BENCH_CONCAT_IMPL(a, b)

#ifdef __COUNTER__
#    define CY_BENCH_UNIQUE(prefix) CY_BENCH_CONCAT(prefix, __COUNTER__)
#else
#    define CY_BENCH_UNIQUE(prefix) CY_BENCH_CONCAT(prefix, __LINE__)
#endif

/// The iteration count the runner chose for this run. The body loops over it.
#define CY_BENCH_ITERATIONS cy_bench_iterations

/// Keep a value the optimiser would delete.
#define CY_BENCH_KEEP(value) ::cy::bench::keep(value)

#define CY_BENCHMARK_IMPL(name, description, first, fn, reg)                           \
    static void fn(std::uint64_t CY_BENCH_ITERATIONS);                                 \
    static const ::cy::bench::Registration reg{(name), (description), &(fn), (first)}; \
    static void fn([[maybe_unused]] std::uint64_t CY_BENCH_ITERATIONS)

/// Declare a benchmark.
///
///     CY_BENCHMARK("ecs/iterate-1m", "…what it measures, and what a regression would mean…") {
///         for (std::uint64_t i = 0; i < CY_BENCH_ITERATIONS; ++i) { … }
///         CY_BENCH_KEEP(result);
///     }
#define CY_BENCHMARK(name, description)                                        \
    CY_BENCHMARK_IMPL(name, description, ::cy::bench::kDefaultFirstIterations, \
                      CY_BENCH_UNIQUE(cy_bench_body_), CY_BENCH_UNIQUE(cy_bench_registration_))

/// Declare a benchmark whose one iteration is expensive — milliseconds rather than nanoseconds —
/// so the runner starts scaling from `first` iterations rather than from a thousand.
#define CY_BENCHMARK_STARTING_AT(name, description, first)                       \
    CY_BENCHMARK_IMPL(name, description, first, CY_BENCH_UNIQUE(cy_bench_body_), \
                      CY_BENCH_UNIQUE(cy_bench_registration_))

#endif  // CY_BENCH_BENCH_H
