# src/rendering/culling/tests/ — the culling suite. Section 4.4 of the M3 tasks.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing, and the taxonomy is read back out
# of tests/ rather than restated, because a budget copied into a second file is a budget that drifts.
#
#   render_culling  every case at unit cost. Each builds a real spatial index and a real frustum:
#                   the subject is the join between the dense arrays, the tree and the frustum test,
#                   and a mock of any of them would test the mock.
set_property(GLOBAL PROPERTY CY_RENDER_CULLING_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_render_culling_declare_tests)
    get_property(cy_culling_tests_dir GLOBAL PROPERTY CY_RENDER_CULLING_TESTS_DIR)

    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_unit)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_unit)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/rendering/culling/tests: the 'unit' entry of the test taxonomy could not be read "
            "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
            "deferral around it should go away — declare the suites from tests/unit/ instead.")
    endif()
    set(CY_TEST_BUDGET_unit "${budget}")
    set(CY_TEST_TIMEOUT_unit "${timeout}")

    cy_add_test(
        NAME render_culling
        KIND unit
        SOURCES
            "${cy_culling_tests_dir}/test_cull.cpp"
            "${cy_culling_tests_dir}/test_lod.cpp"
        DEPENDENCIES cy::rendering-culling)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_render_culling_declare_tests)
