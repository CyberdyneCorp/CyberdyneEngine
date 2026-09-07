# src/rendering/denoise/tests/ — the denoising suites. Task 9.3.
#
# Declared through the deferral src/rendering/culling/tests/ uses: cy_add_test() is defined by
# tests/CMakeLists.txt, which the top-level file adds after src/, so the declaration waits until the
# end of the top-level directory's processing and the taxonomy is read back out of tests/ rather
# than restated.
#
#   render_denoise         the signal table, the priced quality ladder, the bypass, and the refusals.
#                          Arithmetic and small buffers, so it fits the unit budget.
#   render_denoise_filter  the four stages and every scenario `denoising` names, plus the structural
#                          case that keeps "one filter" true. An a-trous cascade over a 32x32 image
#                          with real guidance weights costs more than 1 ms of CPU, and shrinking the
#                          image below the filter's own reach would measure the border instead of
#                          the filter — so these are integration tests and say why.
set_property(GLOBAL PROPERTY CY_RENDER_DENOISE_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_render_denoise_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_RENDER_DENOISE_TESTS_DIR)

    foreach(kind unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/denoise/tests: the '${kind}' entry of the test taxonomy could not "
                "be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME render_denoise
        KIND unit
        SOURCES "${tests_dir}/test_signals.cpp"
        DEPENDENCIES cy::rendering-denoise)

    cy_add_test(
        NAME render_denoise_filter
        KIND integration
        SOURCES
            "${tests_dir}/test_filter.cpp"
            "${tests_dir}/test_one_filter.cpp"
        DEPENDENCIES cy::rendering-denoise)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_render_denoise_declare_tests)
