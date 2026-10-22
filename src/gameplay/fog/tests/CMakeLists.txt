# src/gameplay/fog/tests/ — fog of war's simulation suite.
#
#   gameplay_fog   unit. The sight disc on the integer grid, explored against visible, shared
#                  vision, enemy units hidden and structures remembered as ghosts, the refusals that
#                  change nothing, and a committed digest every CI leg must reproduce.
set_property(GLOBAL PROPERTY CY_GAMEPLAY_FOG_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_gameplay_fog_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_GAMEPLAY_FOG_TESTS_DIR)
    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_unit)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_unit)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/gameplay/fog/tests: the 'unit' entry of the test taxonomy could not be read from "
            "tests/CMakeLists.txt.")
    endif()
    set(CY_TEST_BUDGET_unit "${budget}")
    set(CY_TEST_TIMEOUT_unit "${timeout}")
    cy_add_test(
        NAME gameplay_fog
        KIND unit
        SOURCES "${tests_dir}/test_fog_of_war.cpp"
        DEPENDENCIES cy::gameplay-fog)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_gameplay_fog_declare_tests)
