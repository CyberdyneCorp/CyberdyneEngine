# benchmarks/ecs/ — the ECS's per-entity costs.
#
# Task 1.6, carried forward from M2. `tools/roadmap/milestones/m2.toml` records that the roadmap's
# "a bulk-copy activation is benchmarked" was met by a correctness test with a time budget rather
# than by a committed ns/entity figure, because no benchmark runner existed for the ECS — only for
# the job system. `cy_add_benchmark(NAME ecs ...)` is the fix it names, and this is it.
#
# One binary, five bodies, each measuring one entity's worth of work so the runner's ns/op is
# ns/entity. The thresholds are in benchmarks/baseline.json as ratios against the calibration
# workload, which is what lets the same numbers gate a shared runner and a workstation.

cy_add_benchmark(
    NAME         ecs
    SOURCES      bench_ecs.cpp
    DEPENDENCIES cy::ecs cy::core-reflect)
