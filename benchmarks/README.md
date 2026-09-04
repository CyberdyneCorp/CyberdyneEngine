# `benchmarks/`

Performance benchmarks with recorded baselines and regression thresholds, run by `just test-bench`
and nightly in CI. A regression is reported with the commit range so it can be bisected.

A benchmark states what it measures and what budget it defends. A number with no budget is a
measurement, not a gate — so a benchmark with no baseline entry fails the run rather than printing
itself.

## The pieces

| Path | What it is |
|---|---|
| `harness/` | Registration, the timing loop, the calibration, and the results file. `cy::bench-harness`. |
| `micro/` | Benchmarks with no engine dependency. Today: one, and it measures the harness. |
| `ecs/` | The ECS's per-entity costs: query iteration, random access, spawn, block activation, deferred structural change. |
| `tools/compare.py` | Turns a run into a pass or a failure against the baseline. |
| `baseline.json` | The thresholds. A reviewed file: changing it is recording an intentional trade-off. |

## Writing one

```cpp
#include <cy/bench/bench.h>

CY_BENCHMARK("ecs/iterate-1m",
             "Iteration over a million entities in four archetypes. A regression here means the "
             "chunk layout or the query's inner loop, and it is felt by every system in the frame.") {
    for (std::uint64_t i = 0; i < CY_BENCH_ITERATIONS; ++i) {
        // …
    }
    CY_BENCH_KEEP(result);
}
```

The description is a required argument, because `testing-and-quality` requires each benchmark to
declare what a regression would mean — a threshold that fires at three in the morning is only
actionable if it says what it is defending.

## Why the threshold is a ratio

Each run measures a calibration workload — a dependent multiply-add chain, latency-bound and with no
memory traffic — and every result is reported both in nanoseconds and as a ratio against it. The
baseline stores the ratio.

A nanosecond baseline is a property of the machine that recorded it, so it can only be compared
against that machine; on any other it is noise, and a threshold that is noise gets switched off.
The ratio divides the machine out for anything ALU- or latency-bound.

It does **not** divide out cache and memory behaviour. A benchmark dominated by memory traffic
needs its baseline recorded on the reference machine, and its tolerance set with that in mind.

That is why `ecs/` carries wider tolerances than the 20% default: its bodies are bound by the chunk
allocator and by 2.4 MB of column data that does not fit in cache, and the calibration workload
divides out neither. They were measured at 25% for `ecs/query-iterate` and 35% for the four that
allocate or chase pointers — chosen from the spread of repeated runs on the recording machine, wide
enough not to fire on a busy agent and far too narrow for a doubling to hide in.

**Not every runner in `benchmarks/` is one of these.** `cy_bench_jobs_throughput` measures a thread
pool against a serial baseline, owns its own thresholds, and has its own recipe (`just
test-bench-jobs`); `just test-bench` recognises it by the fact that it does not answer `--list` and
leaves it alone.

## Running

```
just test-bench                          # build, measure, compare against baseline.json
just test-bench --profile profile        # the profile worth quoting: assertions off
just test-bench --record                 # rewrite the thresholds — a reviewed step
just test-bench --filter ecs             # forwarded to the runner
```

The measurement is the fastest of five repetitions, each at least 20 ms long, at an iteration count
the harness scales to reach that. The minimum is the estimate of the work: nothing makes the work
faster than it is, and everything else on the machine makes some repetitions slower.

## The thresholds file

`just test-bench --record` **merges**: it rewrites the entries this run measured and leaves the rest
alone, because the recipe calls it once per runner and a record that replaced the file would leave
the baseline holding only the last runner's benchmarks. An entry that is genuinely obsolete is
deleted by hand, which is the right amount of friction for discarding a threshold.
`python3 benchmarks/tools/compare.py --selftest` checks that and the comparison's other decisions;
`just test-bench` runs it before it measures anything.

## What is not here yet

`testing-and-quality` names three acceptance scenarios — the strategy stress, the control handover,
and the headless server — and calls the first the primary architectural test, since a
single-character scenario does not distinguish a data-oriented gameplay framework from an
object-oriented one. Each needs an ECS, a renderer and a network stack; each joins this directory at
the milestone that lands them. What M0 owes is a harness whose threshold is checked, proved by a
benchmark simple enough that a failure can only mean the harness.

**Governed by**: `testing-and-quality`.
