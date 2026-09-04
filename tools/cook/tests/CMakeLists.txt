# tools/cook/tests/ — the cook path's suite. M2 task 3.2.13.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* tools/, so that function does not exist while this file is read. The
# declaration is therefore deferred to the end of the top-level directory's processing, and the
# taxonomy is read back out of tests/ rather than restated. Every module with its own suites does
# the same, for the same reason.
#
# It is an integration suite: it builds a `World`, mounts a filesystem, cooks documents and hashes
# payloads. None of that fits a millisecond, and shrinking it to fit would be testing something
# smaller than the cook path.

set_property(GLOBAL PROPERTY CY_COOK_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_cook_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_COOK_TESTS_DIR)

    foreach(kind IN ITEMS integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "tools/cook/tests: the '${kind}' entry of the test taxonomy could not be read from "
                "tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
                "deferral around it should go away — declare the suite from tests/integration/ "
                "instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME cook_path
        KIND integration
        SOURCES "${tests_dir}/test_cook_path.cpp"
        DEPENDENCIES cy::cook-pipeline cy::core-assets)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_cook_declare_tests)
