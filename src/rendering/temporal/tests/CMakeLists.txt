# src/rendering/temporal/tests/ — the temporal framework suite. Section 8 of the M7 tasks.
#
#   render_temporal  every case at unit cost. The cases that matter are the ones where TWO consumers
#                    are registered and have to agree: one jitter, one classification, one
#                    invalidation reaching both in the same frame. A suite with one consumer would
#                    pass against five separate frameworks, which is the arrangement this module
#                    exists to prevent.
set_property(GLOBAL PROPERTY CY_RENDER_TEMPORAL_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_render_temporal_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_RENDER_TEMPORAL_TESTS_DIR)

    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_unit)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_unit)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/rendering/temporal/tests: the 'unit' entry of the test taxonomy could not be read "
            "from tests/CMakeLists.txt.")
    endif()
    set(CY_TEST_BUDGET_unit "${budget}")
    set(CY_TEST_TIMEOUT_unit "${timeout}")

    cy_add_test(
        NAME render_temporal
        KIND unit
        SOURCES
            "${tests_dir}/test_framework.cpp"
            "${tests_dir}/test_jitter.cpp"
            "${tests_dir}/test_motion.cpp"
        DEPENDENCIES cy::rendering-temporal)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_render_temporal_declare_tests)
