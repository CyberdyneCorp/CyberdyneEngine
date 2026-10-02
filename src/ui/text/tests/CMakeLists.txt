# src/ui/text/tests/ — `unit.ui_text`: the built-in font's glyphs, and a label's text measured and
# painted into the primitive stream through a real text server. No device, no file.
set_property(GLOBAL PROPERTY CY_UI_TEXT_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_ui_text_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_UI_TEXT_TESTS_DIR)
    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests" DEFINITION CY_TEST_BUDGET_unit)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests" DEFINITION CY_TEST_TIMEOUT_unit)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR "src/ui/text/tests: the 'unit' entry of the taxonomy is missing.")
    endif()
    set(CY_TEST_BUDGET_unit "${budget}")
    set(CY_TEST_TIMEOUT_unit "${timeout}")
    cy_add_test(
        NAME ui_text
        KIND unit
        SOURCES "${tests_dir}/test_text.cpp"
        DEPENDENCIES cy::ui-text)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_ui_text_declare_tests)
