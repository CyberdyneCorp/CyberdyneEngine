# src/rendering/fog_of_war/tests/ — fog of war, drawn, on the host and on a device.
#
#   render_fog_of_war   KIND unit. The visibility field against `cy::gameplay::fog::FogOfWar`'s
#                       grid cell for cell, the fade, the smoothing's continuity, the shading, the
#                       minimap overlay and the constants' reconstruction of a world position. No
#                       device.
#
#   fog_of_war          KIND render, Vulkan only. `pipeline_test::FrameScene` shaped into a strategy
#                       field with blue's fog in the frame: the device texture against the grid, the
#                       dispatch against the host twins at every pixel, explored and unexplored
#                       ground, enemy units by the simulation's sight, smooth edges, the fade, and
#                       off against a committed reference. It compiles
#                       src/rendering/pipeline/tests/frame_scene.cpp and tests/render/golden.cpp.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory for the reason
# src/rendering/pipeline/tests/CMakeLists.txt gives.
set_property(GLOBAL PROPERTY CY_FOG_OF_WAR_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_fog_of_war_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_FOG_OF_WAR_TESTS_DIR)
    foreach(kind IN ITEMS unit render)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/fog_of_war/tests: the '${kind}' entry of the test taxonomy could "
                "not be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME render_fog_of_war
        KIND unit
        SOURCES "${tests_dir}/test_visibility_field.cpp"
        DEPENDENCIES cy::rendering-fog-of-war cy::gameplay-fog)

    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    set(pipeline_tests "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests")
    cy_add_test(
        NAME fog_of_war
        KIND render
        SOURCES
            "${tests_dir}/test_fog_of_war_device.cpp"
            "${pipeline_tests}/frame_scene.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-fog-of-war cy::gameplay-fog cy::rendering-pipeline
                     cy::rendering-particles cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_fog_of_war
                               PRIVATE "${tests_dir}" "${pipeline_tests}"
                                       "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_test_render_fog_of_war
                               PRIVATE CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
                                       CY_FOG_OF_WAR_TEST_DIR="${tests_dir}")
    set_tests_properties(render.fog_of_war PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_fog_of_war_declare_tests)
