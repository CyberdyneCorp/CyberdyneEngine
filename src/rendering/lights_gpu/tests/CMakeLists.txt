# src/rendering/lights_gpu/tests/ — many lights and area lights, on the host and on a device.
#
#   render_lights_gpu   KIND unit. The alias table the candidates are drawn from against the
#                       lights' power, the constants' layout, the LTC texels' layout, the cluster
#                       element's, and the refusal of a stochastic path with no denoiser — on the
#                       null device. No GPU.
#
#   area_lights         KIND render, Vulkan only. `pipeline_test::FrameScene` lit by rect, disc,
#                       sphere and tube lights: the frame without one against a committed reference
#                       drawn by the frame shader before area lights, every lit floor pixel against
#                       the host's `shade_area_light`, the table's half, mobility, and the area-lit
#                       room's picture.
#
#   many_lights         KIND render, Vulkan only. The device's light lists against the host's, and
#                       the picture they draw; a thousand lights at 1080p against a stated budget;
#                       the stochastic path's estimate against a sum over every light, its
#                       denoiser against `Denoiser::denoise`, its noise and stability, the two paths
#                       against each other, an area light's soft shadow, and the coloured lights'
#                       picture.
#
# Both device suites compile src/rendering/pipeline/tests/frame_scene.cpp and
# tests/render/golden.cpp. Declared with cy_add_test(), deferred to the end of the top-level
# directory for the reason src/rendering/pipeline/tests/CMakeLists.txt gives.
set_property(GLOBAL PROPERTY CY_LIGHTS_GPU_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_lights_gpu_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_LIGHTS_GPU_TESTS_DIR)
    foreach(kind IN ITEMS unit render)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/lights_gpu/tests: the '${kind}' entry of the test taxonomy could "
                "not be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME render_lights_gpu
        KIND unit
        SOURCES "${tests_dir}/test_lights_gpu.cpp"
        DEPENDENCIES cy::rendering-lights-gpu cy::rhi-null)

    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    set(pipeline_tests "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests")
    foreach(suite IN ITEMS area_lights many_lights)
        cy_add_test(
            NAME ${suite}
            KIND render
            SOURCES
                "${tests_dir}/test_${suite}_device.cpp"
                "${pipeline_tests}/frame_scene.cpp"
                "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
            DEPENDENCIES cy::rendering-lights-gpu cy::rendering-pipeline cy::rendering-particles
                         cy::rendering-denoise cy::rhi-null cy::rhi-vulkan)
        target_include_directories(cy_test_render_${suite}
                                   PRIVATE "${tests_dir}" "${pipeline_tests}"
                                           "${CMAKE_SOURCE_DIR}/tests/render")
        target_compile_definitions(cy_test_render_${suite}
                                   PRIVATE CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
                                           CY_LIGHTS_GPU_TEST_DIR="${tests_dir}")
        set_tests_properties(render.${suite} PROPERTIES
                             SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
    endforeach()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_lights_gpu_declare_tests)
