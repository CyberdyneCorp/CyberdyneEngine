# src/core/jobs/ — layer 0: the one job system that owns every worker thread in the process, and
# the access declarations that make parallel execution safe by construction.
#
# Section 3.2. Governed by `core-jobs-and-concurrency`. Thread roles, Read/Write/Exclude access
# declarations and the conflict checker, task context, coroutines as the asynchronous model,
# cancellation, priority classes and deadlines, chunked long-running work, deterministic scheduling.
#
# The declaration model lands before there is a system to schedule, deliberately (design.md §3):
# safety-by-construction constrains how a system is written, so it cannot be retrofitted to systems
# written without it. Synthetic systems exercise it until M2 brings real ones.
#
# A worker never blocks on I/O or the GPU, and there is a test that tries to and fails.

# std::thread and std::shared_mutex. An imported target with no layer, so it passes the layer check
# unremarked.
find_package(Threads REQUIRED)

# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_core_jobs
    LAYER core
    ALIAS cy::core-jobs
    TYPE STATIC
    SOURCES
        src/access.cpp
        src/async.cpp
        src/cancellation.cpp
        src/command_queue.cpp
        src/coroutine.cpp
        src/diagnostics.cpp
        src/job_system.cpp
        src/schedule.cpp
        src/scratch.cpp
        src/thread_role.cpp
        src/types.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PUBLIC_DEPENDENCIES cy::core-base
    # cy::core-diagnostics is PRIVATE and reached from exactly one translation unit
    # (src/diagnostics.cpp), so nothing that includes <cy/core/jobs/job_system.h> acquires the
    # trace's headers. src/core/CMakeLists.txt already registers diagnostics before jobs, so this
    # dependency runs with the declared order rather than against it.
    PRIVATE_DEPENDENCIES Threads::Threads cy::core-diagnostics)

if(CY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
