# benchmarks/gameplay/ — the gameplay framework's per-operation costs.
#
# M10 task 6.3, closing `m9:gameplay-benchmarks`. `gameplay-framework`'s performance table "SHALL be
# benchmarked rather than asserted", and M9's closing gate found `benchmarks/baseline.json` holding
# six entries of which every one was `ecs/*` or `harness/*`. The declaration that recorded the gap
# names M10 as the rung that closes it; bench_gameplay.cpp says which rows of the table it covers
# and which are structural rather than measurable.
#
# One binary, five bodies, each measuring one command or one entity's worth of work so the runner's
# ns/op is the unit the requirement is written in. The thresholds are in benchmarks/baseline.json as
# ratios against the calibration workload, which is what lets the same numbers gate a shared runner
# and a workstation.

cy_add_benchmark(
    NAME         gameplay
    SOURCES      bench_gameplay.cpp
    DEPENDENCIES cy::gameplay)
