# tests/harness/ — the framework seam and the fixtures, linked by every test binary.
#
# This is the one place doctest is named. cy::dep::doctest carries
# DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS (cmake/dependencies.cmake), which is what makes
# the framework usable under -fno-exceptions, and it is a PUBLIC dependency here because
# cy/test/test.h includes doctest's header: a test binary compiles against it.

cy_add_module(
    NAME  cy_test_harness
    LAYER tools
    ALIAS cy::test-harness
    SOURCES
        src/budget.cpp
        src/fixtures.cpp
        src/host_blocking.cpp
        src/main.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::dep::doctest)

# ================================================================================================
# THE HARNESS'S PREMISE, UNDER A HOST MADE BUSY ON PURPOSE
# ================================================================================================
#
# M11.c's seventh close. The stall ceiling in src/budget.cpp assumes a quiet host and excuses no
# uninterruptible wait; the ledger criteria that run timing-sensitive suites state that premise and
# run through `cy_quiet_host` (tools/quiet-host/), which fails with "host too busy:" when the host
# is not quiet before or during the run. `quiet_host_test.py` runs the wrapper around trivial
# commands with four niced spinners it starts on purpose and kills by PID, and requires the wrapper
# to fail with that reason; removing either check turns its leg red. The two legs that need a quiet
# host to begin with exit 3 (NOT EVALUATED, CTest's skip) on a busy one and say why, each a separate
# entry so that skip never hides another leg's verdict:
#
#   * `smoke.quiet_host_before`  loads the host BEFORE the command; the command must not run.
#   * `smoke.quiet_host_across`  loads the host once the command has STARTED; the run must fail.
#   * `smoke.quiet_host_own`     the command itself burns four cores; its own load is subtracted,
#                                so a quiet host lets it run and its exit status is the wrapper's.
#
# Declared here rather than beside the wrapper because tools/ is configured before tests/ and the
# taxonomy's timeouts live in tests/CMakeLists.txt. RUN_SERIAL because they load four cores on
# purpose and no other suite should share the machine. The wrapper is Linux-only and built with the
# tools; without it there is nothing to test and the entries are not declared.
if(TARGET cy_quiet_host)
    find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        foreach(leg IN ITEMS before across own)
            add_test(NAME smoke.quiet_host_${leg}
                     COMMAND "${Python3_EXECUTABLE}"
                             "${CMAKE_SOURCE_DIR}/tools/quiet-host/quiet_host_test.py"
                             --wrapper "$<TARGET_FILE:cy_quiet_host>" --leg ${leg}
                     WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
            set_tests_properties(smoke.quiet_host_${leg} PROPERTIES
                                 LABELS "smoke" TIMEOUT "${CY_TEST_TIMEOUT_smoke}" RUN_SERIAL TRUE)
        endforeach()
        set_tests_properties(smoke.quiet_host_across smoke.quiet_host_own
                             PROPERTIES SKIP_RETURN_CODE 3)
    else()
        message(STATUS "tests/harness: smoke.quiet_host_* need python3, so they are not declared.")
    endif()
endif()
