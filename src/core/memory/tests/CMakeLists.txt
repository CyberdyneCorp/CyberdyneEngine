# src/core/memory/tests/ — the memory layer's suites. Section 2.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred: this file defines one function and asks CMake to call it at the end of the
# top-level directory's processing, by which time the taxonomy has been declared. (A deferred
# add_subdirectory() is rejected outright by CMake — "Subdirectories may not be created during
# deferred execution" — so the deferral has to be of the declaration, not of the directory.)
#
# src/core/values/tests/ and src/core/reflect/tests/ hit the same wall and solved it the same way.
# It is a workaround for a layout problem M1 should settle: either move `add_subdirectory(tests)`
# above `add_subdirectory(src)` in the top-level CMakeLists, or lift the taxonomy into a cmake/
# module both sides include. Raised in src/core/memory/README.md.
#
# Sources are absolute, because the deferred call runs in the top-level directory's scope and a
# relative path would be resolved against that directory rather than against this one.
#
# THE SPLIT BETWEEN UNIT AND INTEGRATION IS THE TAXONOMY'S, AND IT IS ABOUT COST. A unit test has a
# millisecond, does no I/O and starts no thread. `memory_threads` starts sixteen of them and
# `memory_diagnostics` writes a trace file, so both are integration; `memory_containers` holds the
# cases that need hundreds of thousands of operations to demonstrate anything, which do not fit a
# millisecond in the Debug configuration — where the budget guard is what found them.

set_property(GLOBAL PROPERTY CY_MEMORY_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_memory_declare_tests)
    get_property(cy_memory_tests_dir GLOBAL PROPERTY CY_MEMORY_TESTS_DIR)

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
                "src/core/memory/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
                "the deferral around it should go away — declare the suites directly from "
                "tests/unit/memory/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME memory
        KIND unit
        SOURCES
            "${cy_memory_tests_dir}/test_allocators.cpp"
            "${cy_memory_tests_dir}/test_budgets.cpp"
            "${cy_memory_tests_dir}/test_containers.cpp"
            "${cy_memory_tests_dir}/test_lifetimes.cpp"
            "${cy_memory_tests_dir}/test_pools.cpp"
            "${cy_memory_tests_dir}/test_pressure.cpp"
        DEPENDENCIES cy::core-memory)

    cy_add_test(
        NAME memory_containers
        KIND integration
        SOURCES "${cy_memory_tests_dir}/test_container_stress.cpp"
        DEPENDENCIES cy::core-memory)

    cy_add_test(
        NAME memory_threads
        KIND integration
        SOURCES "${cy_memory_tests_dir}/test_threads.cpp"
        DEPENDENCIES cy::core-memory)

    # Links cy::core-diagnostics as well: the test opens the one trace and reads back its
    # statistics, which is the point — the memory layer emits onto the trace that already exists
    # rather than onto one of its own.
    cy_add_test(
        NAME memory_diagnostics
        KIND integration
        SOURCES "${cy_memory_tests_dir}/test_diagnostics.cpp"
        DEPENDENCIES cy::core-memory cy::core-diagnostics)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_memory_declare_tests)
