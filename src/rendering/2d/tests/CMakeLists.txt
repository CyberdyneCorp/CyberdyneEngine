# src/rendering/2d/tests/ — the 2D pipeline's suite. M8.b task 9.5.
#
# TWO SUITES. `unit` holds the arithmetic — sorting, batching, tilemap rebuilds, shadow geometry, the
# distance field and the camera — over arrays of a few dozen entries; no device, no window, no file.
# `integration` holds the one case the specification states at five thousand sprites, which measured
# just over the unit tier's millisecond and belongs in the tier above rather than on its edge.

set_property(GLOBAL PROPERTY CY_RENDERING_2D_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_rendering_2d_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_RENDERING_2D_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR "src/rendering/2d/tests: the '${kind}' taxonomy entry is missing.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME rendering_2d
        KIND unit
        SOURCES
            "${tests_dir}/test_sprite.cpp"
            "${tests_dir}/test_tilemap.cpp"
            "${tests_dir}/test_lighting.cpp"
        DEPENDENCIES cy::rendering-2d)

    # The five-thousand-sprite case the specification states at its own scale. It measured 1.01 ms —
    # over the unit tier's budget by a hair — and hard rule 7 puts a case that expensive in the tier
    # above rather than shaving it down to a number the specification does not state.
    cy_add_test(
        NAME rendering_2d_scale
        KIND integration
        SOURCES
            "${tests_dir}/test_batch_scale.cpp"
        DEPENDENCIES cy::rendering-2d)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_rendering_2d_declare_tests)
