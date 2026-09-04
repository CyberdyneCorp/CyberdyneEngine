# tests/smoke/ — the engine starts, does its job, and exits cleanly.
#
# Thirty seconds per test case, and the only suite that runs a whole binary rather than linking part
# of one. Both tests here spawn a program and read what it produced, which is why the budget is what
# it is: they measure the artefact, not a function.
#
#   empty_sample   task 4.1.4 — samples/00-empty headless, a clean exit, and a trace with the shape
#                  the capture format declares.
#   startup_order  task 3.4.4 — the runtime's startup and shutdown order, compared across a hundred
#                  processes. It belongs to this suite rather than to the unit set because the claim
#                  is about a process's whole lifetime and only separate processes can make it.
#   headless_host  tasks 5.3 and 5.4 — samples/01-headless-host: a clean exit and a budget report
#                  with real figures in it, then the same run a hundred times over, compared line
#                  for line. It is the M1 milestone gate.
#   headless_sim   M2 task 5.3 — samples/02-headless-sim: the flattening decision, the reference
#                  fixup and the coherence invariant read off the run's own report, the whole report
#                  compared across twenty processes, and the state hash checked against a snapshot
#                  restore. It is the M2 milestone gate.
#
# The binaries under test are passed as their generator expressions, so a test cannot be run against
# a path that does not exist or against a stale copy somewhere else in the tree.

cy_add_test(NAME empty_sample KIND smoke
            SOURCES test_empty_sample.cpp
            DEPENDENCIES cy::core-diagnostics
            DEFINITIONS CY_SAMPLE_EMPTY="$<TARGET_FILE:cy_sample_empty>")

cy_add_test(NAME startup_order KIND smoke
            SOURCES test_startup_order.cpp
            DEFINITIONS CY_STARTUP_PROBE="$<TARGET_FILE:cy_runtime_startup_probe>")

cy_add_test(NAME headless_host KIND smoke
            SOURCES test_headless_host.cpp
            DEFINITIONS CY_SAMPLE_HEADLESS_HOST="$<TARGET_FILE:cy_sample_headless-host>")

cy_add_test(NAME headless_sim KIND smoke
            SOURCES test_headless_sim.cpp
            DEFINITIONS CY_SAMPLE_HEADLESS_SIM="$<TARGET_FILE:cy_sample_headless-sim>")
