# src/core/values/tests/ — the value layer's suites. Section 1.3.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred: this file defines one function and asks CMake to call it at the end of the
# top-level directory's processing, by which time the taxonomy has been declared. (A deferred
# add_subdirectory() is rejected outright by CMake — "Subdirectories may not be created during
# deferred execution" — so the deferral has to be of the declaration, not of the directory.)
#
# The alternative is what diagnostics/ had to do at M0: declare each binary with cy_add_module() and
# register it with a bare add_test(). That works, and it opts the suite out of the per-test budget,
# which is the one thing the wrapper exists to enforce. Deferring keeps the budget.
#
# Sources are absolute, because the deferred call runs in the top-level directory's scope and a
# relative path would be resolved against that directory rather than against this one.
#
# The split between unit and integration is the taxonomy's, and it is about cost rather than about
# subject: a unit test has a millisecond and does no I/O and starts no thread. `values_threads`
# starts eight of them and `values_diagnostics` writes a trace file, so both are integration.

# This directory's path travels to the deferred call in a global property. Neither an ordinary
# variable nor a deferred-call argument survives the trip: the call runs in the top-level scope,
# which is not a child of this one, and a global property is the one place both scopes can see.
set_property(GLOBAL PROPERTY CY_VALUES_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_values_declare_tests)
    get_property(cy_values_tests_dir GLOBAL PROPERTY CY_VALUES_TESTS_DIR)

    # cy_add_test() reads the taxonomy from variables that tests/CMakeLists.txt sets in its own
    # directory scope, and this function runs in the top-level scope, which is not a child of it.
    # They are read back from the directory that defines them rather than restated here: a budget
    # copied into a second file is a budget that drifts, and the taxonomy is the one thing about a
    # suite that must not.
    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/core/values/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
                "the deferral around it should go away — declare the suites directly from "
                "tests/unit/values/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME values
        KIND unit
        SOURCES
            "${cy_values_tests_dir}/test_asset_id.cpp"
            "${cy_values_tests_dir}/test_callable.cpp"
            "${cy_values_tests_dir}/test_events.cpp"
            "${cy_values_tests_dir}/test_handle.cpp"
            "${cy_values_tests_dir}/test_name.cpp"
            "${cy_values_tests_dir}/test_var.cpp"
        DEPENDENCIES cy::core-values)

    cy_add_test(
        NAME values_threads
        KIND integration
        SOURCES "${cy_values_tests_dir}/test_threads.cpp"
        DEPENDENCIES cy::core-values)

    # Links cy::core-diagnostics as well: the test opens the one trace and reads back its
    # statistics, which is the point — the value layer emits onto the trace that already exists
    # rather than onto one of its own.
    cy_add_test(
        NAME values_diagnostics
        KIND integration
        SOURCES "${cy_values_tests_dir}/test_diagnostics.cpp"
        DEPENDENCIES cy::core-values cy::core-diagnostics)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_values_declare_tests)

# The compile-time half of "asset ids are distinct from handles": five programs that must not
# compile, and one that must. Driven by Python because the assertion is about the compiler's exit
# status, which CMake cannot express as a target.
find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)

add_test(NAME integration.values_type_separation
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_type_separation.py"
            --compiler "${CMAKE_CXX_COMPILER}"
            --include "${CMAKE_CURRENT_SOURCE_DIR}/../include"
            --include "${CMAKE_CURRENT_SOURCE_DIR}/../../base/include"
            --sources "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail")
set_tests_properties(integration.values_type_separation PROPERTIES
    TIMEOUT 300 LABELS integration)
