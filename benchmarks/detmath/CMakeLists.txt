# benchmarks/detmath/ — the deterministic math module's costs against design §11's budgets
# (openspec/changes/add-deterministic-math, tasks 1.7 and 2.5). The baselines are ratios to the
# harness's calibration workload, as every benchmark here is; benchmarks/README.md says why.
cy_add_benchmark(
    NAME         detmath
    SOURCES      bench_detmath.cpp
    DEPENDENCIES cy::core-detmath)
