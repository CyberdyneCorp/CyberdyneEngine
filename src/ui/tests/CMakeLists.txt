# src/ui/tests/ — CyberUI's suites. M8.b section 9.
#
# TWO SUITES. `unit` builds element trees of a few dozen nodes and runs layout, styling, routing or
# flattening over them — no world, no device, no file; the virtualisation case names a hundred
# thousand rows and realises a dozen, which is the whole point of it and is why it still fits the
# tier's budget. `integration` is the twenty-thousand-element document `ui-system` states, and the
# teardown under that load.

set_property(GLOBAL PROPERTY CY_UI_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_ui_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_UI_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR "src/ui/tests: the '${kind}' entry of the taxonomy is missing.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME ui
        KIND unit
        SOURCES
            "${tests_dir}/test_store.cpp"
            "${tests_dir}/test_layout.cpp"
            "${tests_dir}/test_style.cpp"
            "${tests_dir}/test_interaction.cpp"
            "${tests_dir}/test_paint.cpp"
        DEPENDENCIES cy::ui)

    # The scale `ui-system` states — twenty thousand elements — and the teardown under it hard rule
    # 4 of the milestone's brief asks for. Both are milliseconds of work several times over, which
    # is the tier above rather than the unit tier's budget.
    cy_add_test(
        NAME ui_scale
        KIND integration
        SOURCES
            "${tests_dir}/test_scale.cpp"
        DEPENDENCIES cy::ui)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_ui_declare_tests)
