# src/core/jobs/tests/ — the job system's suites, and the throughput benchmark. Section 3.2.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred to the end of the top-level directory's processing, by which time the
# taxonomy has been declared. A deferred add_subdirectory() is rejected outright by CMake, so the
# deferral has to be of the declaration rather than of the directory. src/core/values/tests/ and
# src/core/reflect/tests/ hit the same wall and solved it the same way; whoever closes M1 should
# settle the layout properly, either by adding tests/ before src/ or by lifting the taxonomy into a
# cmake/ module both sides include.
#
# THE SPLIT BETWEEN UNIT AND INTEGRATION IS ABOUT COST, NOT SUBJECT. A unit test has one
# millisecond and starts no threads, so everything here that starts a job system is integration —
# which is most of it. What stays in the unit suite is the part that is pure data: the access sets
# and the schedule they derive, the scratch arena, the synchronisation primitives' semantics, and
# the cancellation tree.

set_property(GLOBAL PROPERTY CY_JOBS_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_jobs_declare_tests)
    get_property(cy_jobs_tests_dir GLOBAL PROPERTY CY_JOBS_TESTS_DIR)

    # cy_add_test() reads the taxonomy from variables that tests/CMakeLists.txt sets in its own
    # directory scope, and this function runs in the top-level scope, which is not a child of it.
    # They are read back from the directory that defines them rather than restated here: a budget
    # copied into a second file is a budget that drifts.
    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/core/jobs/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
                "the deferral around it should go away — declare the suites directly from "
                "tests/unit/jobs/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME jobs
        KIND unit
        SOURCES
            "${cy_jobs_tests_dir}/test_access.cpp"
            "${cy_jobs_tests_dir}/test_cancellation.cpp"
            "${cy_jobs_tests_dir}/test_scratch.cpp"
            "${cy_jobs_tests_dir}/test_sync.cpp"
        DEPENDENCIES cy::core-jobs)

    cy_add_test(
        NAME jobs_system
        KIND integration
        SOURCES "${cy_jobs_tests_dir}/test_job_system.cpp"
        DEPENDENCIES cy::core-jobs)

    # The synchronisation primitives' timed waits. Integration rather than unit because a
    # hundred-microsecond timeout costs milliseconds once the scheduler has rounded it up — see the
    # file's own header.
    cy_add_test(
        NAME jobs_sync
        KIND integration
        SOURCES "${cy_jobs_tests_dir}/test_sync_timed.cpp"
        DEPENDENCIES cy::core-jobs)

    # The test that tries to block on a worker and fails, plus the async service and the fence that
    # are the alternative to blocking.
    cy_add_test(
        NAME jobs_blocking
        KIND integration
        SOURCES "${cy_jobs_tests_dir}/test_blocking.cpp"
        DEPENDENCIES cy::core-jobs)

    cy_add_test(
        NAME jobs_coroutines
        KIND integration
        SOURCES "${cy_jobs_tests_dir}/test_coroutines.cpp"
        DEPENDENCIES cy::core-jobs)

    cy_add_test(
        NAME jobs_determinism
        KIND integration
        SOURCES "${cy_jobs_tests_dir}/test_determinism.cpp"
        DEPENDENCIES cy::core-jobs)

    cy_add_test(
        NAME jobs_command_queue
        KIND integration
        SOURCES "${cy_jobs_tests_dir}/test_command_queue.cpp"
        DEPENDENCIES cy::core-jobs)

    cy_add_test(
        NAME jobs_schedule
        KIND integration
        SOURCES "${cy_jobs_tests_dir}/test_schedule_run.cpp"
        DEPENDENCIES cy::core-jobs)

    # Links cy::core-diagnostics as well: the suite opens the one trace and reads its statistics
    # back, which is the point — the job system emits onto the trace that already exists rather than
    # onto one of its own.
    cy_add_test(
        NAME jobs_diagnostics
        KIND integration
        SOURCES "${cy_jobs_tests_dir}/test_diagnostics.cpp"
        DEPENDENCIES cy::core-jobs cy::core-diagnostics)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_jobs_declare_tests)

# --- The throughput benchmark -----------------------------------------------------------------
#
# Task 3.2.12. Not a CTest entry: a threshold checked on a laptop under load is a threshold that
# gets switched off within a month, and benchmarks/ states that position for the whole engine. The
# runner compares itself against the committed thresholds beside it and exits non-zero on a
# regression, so `just test-bench-jobs` is a gate rather than a report.
#
# Declared with cy_add_module rather than cy_add_benchmark for the same reason
# src/core/diagnostics/ declares its overhead measurement here: the shared benchmark harness is a
# fixed-iteration microbenchmark runner, and what this measures is a thread pool's throughput
# against a serial baseline, which needs its own loop and its own comparison.
cy_add_module(
    NAME cy_bench_jobs_throughput
    LAYER tools
    TYPE EXECUTABLE
    SOURCES ../bench/throughput.cpp
    PRIVATE_DEFINITIONS
        CY_JOBS_BENCH_THRESHOLDS="${CMAKE_CURRENT_SOURCE_DIR}/../bench/thresholds.json"
    PRIVATE_DEPENDENCIES cy::core-jobs)

set_target_properties(cy_bench_jobs_throughput PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks")
