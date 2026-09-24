# tools/quiet-host/ — layer 7: the quiet-host premise, as a library and as a wrapper.
#
# A wall-clock measurement on a loaded machine measures the machine. Two things in this tree state
# the host they assume and check it rather than excusing what a busy host does to them:
# `samples/10-world --quiet-host` (M11.a's frame budget) and, since M11.c's seventh close, every
# ledger criterion that runs a timing-sensitive test suite. Both use the same check, `cy::host-load`,
# which lived in samples/10-world/ until the suites needed it too.
#
# TWO TARGETS. `cy_host_load` is the check with no `main`: what "quiet" means, read from /proc, with
# the measured program's own load subtracted. `cy_quiet_host` is the wrapper the ledger criteria
# run: `cy_quiet_host -- <command>` waits for a quiet host, runs the command in a session of its
# own, judges the host across the run with the command's whole process tree subtracted, and FAILS
# with "host too busy:" and the numbers when the host was not quiet — never a pass, never a skip.
#
# LAYER tools, which may link any layer: a tool consumes the engine rather than sitting in it.

cy_add_module(
    NAME cy_host_load
    LAYER tools
    ALIAS cy::host-load
    TYPE STATIC
    SOURCES host_load.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    PUBLIC_DEPENDENCIES cy::core-base)

# The wrapper forks, `setsid`s and reads /proc: POSIX and Linux, like the premise it checks. Where it
# is not declared, `just test-quiet-host` says so rather than running the command bare.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

cy_add_module(
    NAME cy_quiet_host
    LAYER tools
    TYPE EXECUTABLE
    SOURCES main.cpp
    PRIVATE_DEPENDENCIES cy::host-load)

# THE CHECK, UNDER A HOST MADE BUSY ON PURPOSE, is `quiet_host_test.py`, declared as
# `smoke.quiet_host_before`, `smoke.quiet_host_across` and `smoke.quiet_host_own` from
# tests/harness/CMakeLists.txt: tools/ is configured before tests/, so the taxonomy's timeouts are
# not readable from here, and the premise is the harness's own.
