# src/rendering/gi_streaming/tests/ — the GI streamed with world cells. Issue #35.
#
# Declared through the deferral src/foliage/tests/ uses: cy_add_test() is defined by
# tests/CMakeLists.txt, which the top-level file adds after src/, so the declaration waits until the
# end of the top-level directory's processing and the taxonomy is read back out of tests/.
#
#   render_gi_streaming  smoke, because it is a route measurement — 160 ticks of a real GI update
#                        and two systems converged to compare — over the integration budget: a real `world::WorldStreaming` over a grid of cooked cells, a
#                        traveller crossing it while cells activate and deactivate, the GI's cards,
#                        fields, emitters and volumes ingested a budget a tick and evicted with
#                        them — every tick timed and the GI held to a system that never streamed.
set_property(GLOBAL PROPERTY CY_RENDER_GI_STREAMING_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_render_gi_streaming_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_RENDER_GI_STREAMING_TESTS_DIR)
    foreach(kind IN ITEMS smoke)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/gi_streaming/tests: the '${kind}' entry of the test taxonomy could "
                "not be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME render_gi_streaming
        KIND smoke
        SOURCES "${tests_dir}/test_gi_streaming.cpp"
        DEPENDENCIES cy::rendering-gi-streaming cy::rendering-gi cy::rendering-raytracing cy::world
                     cy::ecs)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_render_gi_streaming_declare_tests)
