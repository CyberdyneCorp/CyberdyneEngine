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
#
#   render_culling_parallel
#                   the same cull over a real job system, at integration cost. Starting eight
#                   workers costs more CPU time than a unit case is budgeted for before an instance
#                   is tested, and the subject — "each worker producing a local visible list merged
#                   by pointer transfer", in partition order — cannot be observed without them.
set_property(GLOBAL PROPERTY CY_RENDER_CULLING_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_render_culling_declare_tests)
    get_property(cy_culling_tests_dir GLOBAL PROPERTY CY_RENDER_CULLING_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/culling/tests: the '${kind}' entry of the test taxonomy could not be "
                "read from tests/CMakeLists.txt. If the budget variables were renamed, this lookup "
                "and the deferral around it should go away — declare the suites from tests/ "
                "instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME render_culling
        KIND unit
        SOURCES
            "${cy_culling_tests_dir}/test_cull.cpp"
            "${cy_culling_tests_dir}/test_lod.cpp"
        DEPENDENCIES cy::rendering-culling)

    cy_add_test(
        NAME render_culling_parallel
        KIND integration
        SOURCES "${cy_culling_tests_dir}/test_parallel.cpp"
        DEPENDENCIES cy::rendering-culling cy::core-jobs)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_render_culling_declare_tests)
