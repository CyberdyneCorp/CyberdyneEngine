# benchmarks/micro/ — benchmarks with no engine dependency.
#
# Today it holds one, and it measures the harness. That is deliberate: the acceptance scenarios
# `testing-and-quality` names — the strategy stress, the control handover, the headless server —
# need an ECS, a renderer and a network stack, and each joins this directory's siblings at the
# milestone that lands it. What M0 owes is a harness whose threshold is checked, proved by a
# benchmark simple enough that a failure can only mean the harness.

cy_add_benchmark(
    NAME    micro
    SOURCES bench_harness.cpp)
