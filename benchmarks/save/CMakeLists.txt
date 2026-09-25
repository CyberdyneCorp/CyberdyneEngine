# benchmarks/save/ — the large-world save benchmark. M11.e, closing `m11a:save-benchmark`.
#
# `save-and-persistence` says the engine SHALL maintain one: a world of over a million persistent
# objects, most regions unloaded, tens of thousands of dirty records, and an autosave whose cost
# scales with the changes rather than with the world. bench_save.cpp says what one operation is and
# why there are two bodies. The thresholds are in benchmarks/baseline.json as ratios against the
# calibration workload, like every other runner here.

cy_add_benchmark(
    NAME         save
    SOURCES      bench_save.cpp
    DEPENDENCIES cy::world-persistence cy::world cy::save cy::ecs cy::core-reflect)
