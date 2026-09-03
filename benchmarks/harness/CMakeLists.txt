# benchmarks/harness/ — the registry, the timing loop and the calibration every runner links.

cy_add_module(
    NAME  cy_bench_harness
    LAYER tools
    ALIAS cy::bench-harness
    SOURCES src/bench.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include")
