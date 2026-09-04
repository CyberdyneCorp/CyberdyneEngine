# src/core/determinism/tests/ — the clock, the streams, the classification and the hash.
# Tasks 4.2.1 through 4.2.7.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred to the end of the top-level directory's processing, by which time the
# taxonomy has been declared, and the budgets are read back out of tests/ rather than restated: a
# budget copied into a second file is a budget that drifts. src/core/jobs/tests/,
# src/core/reflect/tests/ and src/runtime/tests/ all do the same, for the same reason.
#
# THE SPLIT BETWEEN unit AND integration IS ABOUT COST, NOT SUBJECT. Everything this module does is
# arithmetic and finishes in microseconds; the one exception is `simulation-and-determinism`'s
# no-drift scenario, which names *hours* of ticks and is a quarter of a million calls. That case is
# in the integration suite and its shorter sibling stays in the unit one, with the reason written
# into test_scale.cpp's header. The measured numbers: the four-hour case is 12-15 ms in Debug and
# 1.5 ms in Development, against a unit budget of 1 ms in every profile.

set_property(GLOBAL PROPERTY CY_DETERMINISM_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_determinism_declare_tests)
    get_property(cy_determinism_tests_dir GLOBAL PROPERTY CY_DETERMINISM_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/core/determinism/tests: the '${kind}' entry of the test taxonomy could not be "
                "read from tests/CMakeLists.txt. If the budget variables were renamed, this lookup "
                "and the deferral around it should go away — declare the suites from tests/unit/ "
                "instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME determinism
        KIND unit
        SOURCES
            "${cy_determinism_tests_dir}/test_classification.cpp"
            "${cy_determinism_tests_dir}/test_clock.cpp"
            "${cy_determinism_tests_dir}/test_commit.cpp"
            "${cy_determinism_tests_dir}/test_hash.cpp"
            "${cy_determinism_tests_dir}/test_random.cpp"
        DEPENDENCIES cy::core-determinism)

    cy_add_test(
        NAME determinism_scale
        KIND integration
        SOURCES "${cy_determinism_tests_dir}/test_scale.cpp"
        DEPENDENCIES cy::core-determinism)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_determinism_declare_tests)
