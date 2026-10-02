# src/ui/console/tests/ — `unit.ui_console`: the developer console with no device — its rows, its
# scrollback, its commands, and what it flattens to.
set_property(GLOBAL PROPERTY CY_UI_CONSOLE_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_ui_console_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_UI_CONSOLE_TESTS_DIR)
    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests" DEFINITION CY_TEST_BUDGET_unit)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests" DEFINITION CY_TEST_TIMEOUT_unit)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR "src/ui/console/tests: the 'unit' entry of the taxonomy is missing.")
    endif()
    set(CY_TEST_BUDGET_unit "${budget}")
    set(CY_TEST_TIMEOUT_unit "${timeout}")
    cy_add_test(
        NAME ui_console
        KIND unit
        SOURCES "${tests_dir}/test_console.cpp"
        DEPENDENCIES cy::ui-console)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_ui_console_declare_tests)
