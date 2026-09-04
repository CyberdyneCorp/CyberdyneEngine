# src/rendering/scene/tests/ — the extract stage's suites. Section 4.1 of the M3 tasks.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing, and the taxonomy is read back out
# of tests/ rather than restated, because a budget copied into a second file is a budget that drifts.
#
#   render_extract  every case at unit cost. Each builds a real world, a real scene tree and a real
#                   snapshot buffer — nothing is mocked, because the point of the extractor is the
#                   join between three modules and a mock of any of them would test the mock.
#
#                   The size to watch: the incremental case spawns a hundred entities, which is a
#                   hundred archetype rows and a few chunks. That fits a millisecond comfortably at
#                   `-O0`; the specification's own number is a hundred thousand, and the property
#                   being checked is the RATIO, which a hundred shows just as well.
set_property(GLOBAL PROPERTY CY_RENDER_EXTRACT_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_render_extract_declare_tests)
    get_property(cy_extract_tests_dir GLOBAL PROPERTY CY_RENDER_EXTRACT_TESTS_DIR)

    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_unit)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_unit)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/rendering/scene/tests: the 'unit' entry of the test taxonomy could not be read "
            "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
            "deferral around it should go away — declare the suites from tests/unit/ instead.")
    endif()
    set(CY_TEST_BUDGET_unit "${budget}")
    set(CY_TEST_TIMEOUT_unit "${timeout}")

    cy_add_test(
        NAME render_extract
        KIND unit
        SOURCES
            "${cy_extract_tests_dir}/test_extract.cpp"
            "${cy_extract_tests_dir}/test_state_schema.cpp"
        DEPENDENCIES cy::rendering-scene)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_render_extract_declare_tests)
