# src/runtime/editor_bridge/tests/ — the wire the editor's control socket carries.
#
# Deferred like every other suite declared from src/, because `cy_add_test` is defined in tests/ and
# the top-level directory adds tests/ after src/.
#
# `unit`: every case is a socket pair's worth of bytes and no device, no display and no editor. What
# it cannot check from here is that the OTHER end agrees, and that is what `smoke.editor_window` is
# for — the two together are the whole claim.
set_property(GLOBAL PROPERTY CY_EDITOR_BRIDGE_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_editor_bridge_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_EDITOR_BRIDGE_TESTS_DIR)
    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_unit)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_unit)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/runtime/editor_bridge/tests: the 'unit' entry of the test taxonomy could not be "
            "read from tests/CMakeLists.txt.")
    endif()
    set(CY_TEST_BUDGET_unit "${budget}")
    set(CY_TEST_TIMEOUT_unit "${timeout}")

    cy_add_test(
        NAME editor_bridge
        KIND unit
        SOURCES "${tests_dir}/test_bridge.cpp"
        DEPENDENCIES cy::runtime-editor-bridge)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_editor_bridge_declare_tests)
