# benchmarks/movement/ — the fixed-point kinematic mover against design §11's movement-step budget
# (openspec/changes/add-deterministic-math, task 6.4): 100 000 units with separation in at most
# 4 ms per tick on 8 workers, and at most 2.5x the same kernel in f32. The f32 instantiation of the
# crowd kernel lives here and nowhere in the engine, so the authoritative module compiles no float.
cy_add_benchmark(
    NAME         movement
    SOURCES      bench_movement.cpp
    DEPENDENCIES cy::movement cy::navigation cy::core-jobs)
