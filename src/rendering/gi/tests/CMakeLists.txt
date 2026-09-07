# src/rendering/gi/tests/ — the illumination suites. Tasks 9.1, 9.2 and 9.5.
#
# Declared through the deferral src/rendering/culling/tests/ uses: cy_add_test() is defined by
# tests/CMakeLists.txt, which the top-level file adds after src/, so the declaration waits until the
# end of the top-level directory's processing and the taxonomy is read back out of tests/ rather
# than restated.
#
#   render_gi           the decisions that are arithmetic — detail selection, the exclusion mask,
#                       the far-field ramp, the reflection table, the probe encodings and the GI
#                       budget's control law. Fits the unit budget because none of it traces.
#   render_gi_pipeline  the subsystems on a real room: the sparse field and its scroll, the surface
#                       cache's multi-bounce feedback, adaptive probe placement and the leak the
#                       visibility term stops, the tiers and their confidence, the reflection
#                       capture, the bake's seeding, and a frame of the composed system. Every case
#                       traces real rays through a real field; that is what an integration test is.
#   render_gi_fallback  M7's own exit criterion: ray tracing disabled falls back to software tracing
#                       with no visual discontinuity beyond tolerance. Its own suite because it is a
#                       milestone criterion rather than a unit of behaviour, and because a criterion
#                       buried in a larger file is a criterion nobody runs on its own.
set_property(GLOBAL PROPERTY CY_RENDER_GI_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_render_gi_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_RENDER_GI_TESTS_DIR)

    foreach(kind unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/gi/tests: the '${kind}' entry of the test taxonomy could not be "
                "read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME render_gi
        KIND unit
        SOURCES "${tests_dir}/test_policy.cpp"
        DEPENDENCIES cy::rendering-gi)

    cy_add_test(
        NAME render_gi_pipeline
        KIND integration
        SOURCES
            "${tests_dir}/test_caches.cpp"
            "${tests_dir}/test_field.cpp"
            "${tests_dir}/test_system.cpp"
            "${tests_dir}/test_tiers.cpp"
        DEPENDENCIES cy::rendering-gi)

    cy_add_test(
        NAME render_gi_fallback
        KIND integration
        SOURCES "${tests_dir}/test_fallback.cpp"
        DEPENDENCIES cy::rendering-gi)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_render_gi_declare_tests)
