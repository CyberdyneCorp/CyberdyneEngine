# src/ui/input/tests/ — `unit.ui_input`: the toolkit driven through a real input server by devices
# a case invents — a keyboard, a gamepad — and gameplay suppressed under a modal. No window, no
# device backend: the input server is layer 2 and runs without either.
set_property(GLOBAL PROPERTY CY_UI_INPUT_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_ui_input_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_UI_INPUT_TESTS_DIR)
    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests" DEFINITION CY_TEST_BUDGET_unit)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests" DEFINITION CY_TEST_TIMEOUT_unit)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR "src/ui/input/tests: the 'unit' entry of the taxonomy is missing.")
    endif()
    set(CY_TEST_BUDGET_unit "${budget}")
    set(CY_TEST_TIMEOUT_unit "${timeout}")
    cy_add_test(
        NAME ui_input
        KIND unit
        SOURCES "${tests_dir}/test_ui_input.cpp"
        DEPENDENCIES cy::ui-input)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_ui_input_declare_tests)
