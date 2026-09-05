# src/servers/input/tests/ — CyberInput's suites. Section 4.1.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing and the taxonomy is read back out
# of tests/ rather than restated, exactly as src/servers/render/tests/ does it, because a budget
# copied into a second file is a budget that drifts.
#
#   input_actions   every case at unit cost. There is no window, no display server, no ECS world and
#                   no GPU in any of them — the input server is layer 2 and cannot reach one, which
#                   is what makes `input-and-actions`' "Input SHALL function with no devices
#                   present" a property of the build rather than of a mock. The devices these cases
#                   drive are ones the test invented.
#
#   The one file to read first is test_fixed_tick.cpp. It is design.md §5's requirement and it was
#   written before the resolution it tests; its last case is the negative control that keeps the
#   rest of the file meaningful.
set_property(GLOBAL PROPERTY CY_INPUT_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_input_declare_tests)
    get_property(cy_input_tests_dir GLOBAL PROPERTY CY_INPUT_TESTS_DIR)

    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_unit)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_unit)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/servers/input/tests: the 'unit' entry of the test taxonomy could not be read from "
            "tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
            "deferral around it should go away — declare the suites from tests/unit/ instead.")
    endif()
    set(CY_TEST_BUDGET_unit "${budget}")
    set(CY_TEST_TIMEOUT_unit "${timeout}")

    cy_add_test(
        NAME input_actions
        KIND unit
        SOURCES
            "${cy_input_tests_dir}/test_bindings.cpp"
            "${cy_input_tests_dir}/test_devices.cpp"
            "${cy_input_tests_dir}/test_diagnostics.cpp"
            "${cy_input_tests_dir}/test_fixed_tick.cpp"
            "${cy_input_tests_dir}/test_pipeline.cpp"
            "${cy_input_tests_dir}/test_schemes.cpp"
        DEPENDENCIES cy::servers-input)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_input_declare_tests)
