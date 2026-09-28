# src/rendering/motion_blur/tests/ — per-object motion and motion blur, on the host and on a device.
#
#   rendering_motion_blur   KIND unit. `pipeline::InstanceHistory`: last frame's rows kept, rebased
#                           to the new camera, and discarded on a cut or a recycled slot. No device.
#
#   rendering_motion_blur   KIND integration. The shutter arithmetic, the constants, and the host
#                           reference filter over synthetic frames: a bar moving across a still
#                           background streaks by the shutter's fraction of its motion and leaves
#                           the background untouched, a still foreground stays sharp over a moving
#                           background, the camera and object scales separate, and a closed shutter
#                           copies every pixel. No device. INTEGRATION, NOT UNIT: the reference
#                           filters a 192x24 frame with fifteen taps a pixel, one to two
#                           milliseconds a case, and the unit budget is one.
#
#   motion_blur             KIND render, Vulkan only. The frame the pipeline suites render with a
#                           box moving across it: its velocity is its screen displacement and the
#                           still background's is the camera's alone; temporal antialiasing ghosts
#                           less behind it than with camera motion only; the blur's streak follows
#                           the shutter angle and the background behind it stays sharp; the device
#                           gather is the host reference's; a closed shutter is the frame without the
#                           stage; and the frame without either is byte-identical to committed
#                           references drawn with the frame shaders from before per-object motion.
#                           It compiles src/rendering/pipeline/tests/frame_scene.cpp — the one scene
#                           — and tests/render/golden.cpp for the PNG encoder.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory for the reason
# src/rendering/pipeline/tests/CMakeLists.txt gives.
set_property(GLOBAL PROPERTY CY_MOTION_BLUR_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_motion_blur_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_MOTION_BLUR_TESTS_DIR)
    foreach(kind IN ITEMS unit integration render)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/motion_blur/tests: the '${kind}' entry of the test taxonomy could "
                "not be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME rendering_motion_blur
        KIND unit
        SOURCES "${tests_dir}/test_instance_history.cpp"
        DEPENDENCIES cy::rendering-motion-blur cy::rendering-pipeline)
    cy_add_test(
        NAME rendering_motion_blur
        KIND integration
        SOURCES "${tests_dir}/test_motion_blur.cpp"
        DEPENDENCIES cy::rendering-motion-blur)
    target_include_directories(cy_test_integration_rendering_motion_blur PRIVATE "${tests_dir}")

    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    set(pipeline_tests "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests")
    cy_add_test(
        NAME motion_blur
        KIND render
        SOURCES
            "${tests_dir}/test_motion_blur_device.cpp"
            "${pipeline_tests}/frame_scene.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-motion-blur cy::rendering-pipeline cy::rendering-particles
                     cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_motion_blur
                               PRIVATE "${tests_dir}" "${pipeline_tests}"
                                       "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_test_render_motion_blur
                               PRIVATE CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
                                       CY_MOTION_BLUR_TEST_DIR="${tests_dir}")
    set_tests_properties(render.motion_blur PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_motion_blur_declare_tests)
