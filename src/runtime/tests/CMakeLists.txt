# src/runtime/tests/ — the runtime's startup and shutdown ordering. Task 4.5.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing, by which time the taxonomy has been
# declared; the taxonomy itself is read back out of tests/ rather than restated, because a budget
# copied into a second file is a budget that drifts. src/core/reflect/, values/, memory/, jobs/ and
# config/ all do the same, for the same reason. See the comment in src/core/config/tests/.
#
# `smoke` and not `unit`: the case spawns the probe two hundred times, and a claim about a process's
# whole lifetime is only makeable by separate processes. tests/smoke/test_startup_order.cpp makes the
# neighbouring claim about the eleven stages; this one is here rather than beside it because the
# module stages are src/runtime/'s to fill and this workstream's to prove.

set_property(GLOBAL PROPERTY CY_RUNTIME_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_runtime_declare_tests)
    get_property(cy_runtime_tests_dir GLOBAL PROPERTY CY_RUNTIME_TESTS_DIR)

    foreach(kind IN ITEMS smoke)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/runtime/tests: the '${kind}' entry of the test taxonomy could not be read from "
                "tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
                "deferral around it should go away — declare the suite from tests/smoke/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME module_order
        KIND smoke
        SOURCES "${cy_runtime_tests_dir}/test_module_order.cpp"
        DEFINITIONS CY_STARTUP_PROBE="$<TARGET_FILE:cy_runtime_startup_probe>")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_runtime_declare_tests)
