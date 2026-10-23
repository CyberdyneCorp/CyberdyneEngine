# src/ui/toolkit/tests/ — the toolkit's suites. Issue #91, stages 2, 4 and 5.
#
# `unit.ui_toolkit` builds a few widgets at a time on the built-in font — no device, no file — and
# holds each widget's states, focus, text editing, styles, transitions, animations, bindings and
# documents. `integration.ui_toolkit` is what costs more than a unit's millisecond: the ten-thousand
# item list held to a frame budget, the widget gallery's layout against its golden, and a document
# hot-reloaded from a file on disk.
set_property(GLOBAL PROPERTY CY_UI_TOOLKIT_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_ui_toolkit_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_UI_TOOLKIT_TESTS_DIR)
    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR "src/ui/toolkit/tests: the '${kind}' entry of the taxonomy is missing.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME ui_toolkit
        KIND unit
        SOURCES
            "${tests_dir}/test_widgets.cpp"
            "${tests_dir}/test_text_edit.cpp"
            "${tests_dir}/test_input.cpp"
            "${tests_dir}/test_style_animation.cpp"
            "${tests_dir}/test_list_binding.cpp"
            "${tests_dir}/test_document.cpp"
        DEPENDENCIES cy::ui-toolkit)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_ui_toolkit_declare_tests)
