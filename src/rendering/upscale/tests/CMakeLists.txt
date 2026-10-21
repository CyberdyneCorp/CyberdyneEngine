# src/rendering/upscale/tests/ — temporal upscaling, on the host and on a device.
#
#   rendering_upscale   KIND unit. The modes and their render extents, the jitter phase count,
#                       dynamic resolution as the arbiter's allocation times the mode within
#                       bounds, the constants' sign convention and refusals, and the vendor seam:
#                       a vendor upscaler chosen only when present and available. No device.
#
#   rendering_upscale   KIND integration. The host reference run frame after frame over synthetic
#                       scenes: a still scene converges to the supersampled output in every mode,
#                       the samples are placed where the jittered projection put them, a thin line
#                       is stable and keeps its coverage, a moving object leaves no trail, a
#                       revealed background is rebuilt from this frame, a shading change and a
#                       reactive pixel reach the picture at once, a cut reconstructs from this frame
#                       alone, and the sharpening and the resample. Each with a control that shows
#                       the measure sees the failure. No device. INTEGRATION, NOT UNIT: sixty-four
#                       frames of the reference are tens of milliseconds.
#
#   upscale_off         KIND render, Vulkan only. The frame with no upscaler, byte for byte against
#                       references written by the tree from before the upscaler existed: still under
#                       temporal antialiasing, with the camera and a box moving, and with bloom.
#
#   upscale             KIND render, Vulkan only. The pipeline suites' frame rendered at a reduced
#                       extent and reconstructed to its output: convergence against the native
#                       frame in every mode, ghosting behind a moving box, disocclusion, the device
#                       against the host reference, the interface pixel-exact at the output
#                       extent, and dynamic resolution driven by the budget arbiter.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory for the reason
# src/rendering/pipeline/tests/CMakeLists.txt gives.
set_property(GLOBAL PROPERTY CY_UPSCALE_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_upscale_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_UPSCALE_TESTS_DIR)
    foreach(kind IN ITEMS unit integration render)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/upscale/tests: the '${kind}' entry of the test taxonomy could not "
                "be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME rendering_upscale
        KIND unit
        SOURCES "${tests_dir}/test_upscale.cpp"
        DEPENDENCIES cy::rendering-upscale)
    cy_add_test(
        NAME rendering_upscale
        KIND integration
        SOURCES "${tests_dir}/test_upscale_reference.cpp"
        DEPENDENCIES cy::rendering-upscale)
    target_include_directories(cy_test_integration_rendering_upscale PRIVATE "${tests_dir}")

    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    set(pipeline_tests "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests")
    # Links nothing of the upscaler, so it builds against the tree from before it — which is how its
    # references were written. See the file's header.
    cy_add_test(
        NAME upscale_off
        KIND render
        SOURCES
            "${tests_dir}/test_upscale_off.cpp"
            "${pipeline_tests}/frame_scene.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-pipeline cy::rendering-particles cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_upscale_off
                               PRIVATE "${tests_dir}" "${pipeline_tests}"
                                       "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_test_render_upscale_off
                               PRIVATE CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
                                       CY_UPSCALE_TEST_DIR="${tests_dir}")
    set_tests_properties(render.upscale_off PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
    if(NOT EXISTS "${tests_dir}/test_upscale_device.cpp")
        return()
    endif()
    cy_add_test(
        NAME upscale
        KIND render
        SOURCES
            "${tests_dir}/test_upscale_device.cpp"
            "${pipeline_tests}/frame_scene.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-upscale cy::rendering-pipeline cy::rendering-particles
                     cy::rendering-arbiter cy::rendering-motion-blur cy::ui-render cy::ui-text
                     cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_upscale
                               PRIVATE "${tests_dir}" "${pipeline_tests}"
                                       "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_test_render_upscale
                               PRIVATE CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
                                       CY_UPSCALE_TEST_DIR="${tests_dir}")
    set_tests_properties(render.upscale PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_upscale_declare_tests)
