# src/rendering/occlusion/tests/ — ambient occlusion, on the host and on a device.
#
#   rendering_occlusion   KIND unit. `gtao_reference` over two analytic scenes — an open floor
#                         and the same floor meeting a wall — and the cascade's configuration read
#                         off the shared denoiser's table. No device.
#
#   ambient_occlusion     KIND render, Vulkan only. The five dispatches against the host reference
#                         and against `denoise::Denoiser` buffer for buffer, then the frame the
#                         pipeline suites render with the stage switched on: contact darkening,
#                         open floor unchanged, direct light untouched, off identical to absent and
#                         absent identical to the committed pre-change reference in references/,
#                         and a still view still. It compiles
#                         src/rendering/pipeline/tests/frame_scene.cpp — the one scene — rather
#                         than a second, and tests/render/golden.cpp for
#                         the PNG encoder its before/after captures are written with.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory for the reason
# src/rendering/pipeline/tests/CMakeLists.txt gives.
set_property(GLOBAL PROPERTY CY_OCCLUSION_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_occlusion_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_OCCLUSION_TESTS_DIR)
    foreach(kind IN ITEMS unit render)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/occlusion/tests: the '${kind}' entry of the test taxonomy could not "
                "be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME rendering_occlusion
        KIND unit
        SOURCES "${tests_dir}/test_gtao.cpp"
        DEPENDENCIES cy::rendering-occlusion cy::rendering-denoise)
    target_include_directories(cy_test_unit_rendering_occlusion PRIVATE "${tests_dir}")

    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    set(pipeline_tests "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests")
    cy_add_test(
        NAME ambient_occlusion
        KIND render
        SOURCES
            "${tests_dir}/test_occlusion_device.cpp"
            "${pipeline_tests}/frame_scene.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-occlusion cy::rendering-denoise cy::rendering-pipeline
                     cy::rendering-particles cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_ambient_occlusion
                               PRIVATE "${tests_dir}" "${pipeline_tests}"
                                       "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_test_render_ambient_occlusion
                               PRIVATE CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
                                       CY_OCCLUSION_TEST_DIR="${tests_dir}")
    set_tests_properties(render.ambient_occlusion PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_occlusion_declare_tests)
