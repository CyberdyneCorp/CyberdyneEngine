# src/rendering/post/tests/ — the post-process suite. Section 8 of the M7 tasks.
#
#   render_post       every case at unit cost. The chain-order case is the criterion of task 8.4 and
#                     it is asserted against the specification's own numbering rather than against a
#                     list copied from it, so a stage that moves is caught by the assertion and not
#                     by a reader noticing.
#   render_post_bake  integration, because a 33³ grading LUT is 35,937 evaluations of the reference
#                     grade. 33 is the `.cube` standard and the size that ships, so baking a smaller
#                     one to fit a unit budget would certify something other than what runs.
set_property(GLOBAL PROPERTY CY_RENDER_POST_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_render_post_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_RENDER_POST_TESTS_DIR)

    foreach(kind unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/post/tests: the '${kind}' entry of the test taxonomy could not be "
                "read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME render_post
        KIND unit
        SOURCES
            "${tests_dir}/test_chain.cpp"
            "${tests_dir}/test_effects.cpp"
            "${tests_dir}/test_exposure.cpp"
            "${tests_dir}/test_grading.cpp"
            "${tests_dir}/test_temporal_binding.cpp"
        DEPENDENCIES cy::rendering-post)

    cy_add_test(
        NAME render_post_bake
        KIND integration
        SOURCES "${tests_dir}/test_bake.cpp"
        DEPENDENCIES cy::rendering-post)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_render_post_declare_tests)
