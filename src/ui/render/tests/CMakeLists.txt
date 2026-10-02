# src/ui/render/tests/ — the interface renderer on the host, and on a device.
#
#   ui_render   KIND unit. The host half: a primitive round-trips through the row the shader reads,
#               a clip becomes the scissor of the pixels whose centres it holds, a batch becomes one
#               draw, and `shade_reference` / `draw_reference` — the shader transcribed — draw whole
#               pixels byte for byte, in order, blended premultiplied and scissored. No device.
#
#   ui          KIND render, Vulkan only. `pipeline_test::FrameScene` with an interface drawn over it
#               at the frame's `UiAndDebug` stage: nothing to draw is byte-identical to a committed
#               reference drawn before the interface pass existed; the device matches the host
#               reference to one 8-bit step; order, nested clipping and opacity hold byte for byte;
#               and samples/13-rts-selection's HUD with the developer console matches a committed
#               golden image; on an sRGB output the bytes are the interface's colours. It
#               compiles src/rendering/pipeline/tests/frame_scene.cpp — the one scene —
#               tests/render/golden.cpp for the PNG encoder, and the sample's hud.cpp, so the HUD
#               photographed is the HUD the sample draws.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory for the reason
# src/rendering/pipeline/tests/CMakeLists.txt gives.
set_property(GLOBAL PROPERTY CY_UI_RENDER_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_ui_render_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_UI_RENDER_TESTS_DIR)
    foreach(kind IN ITEMS unit render)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/ui/render/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME ui_render
        KIND unit
        SOURCES "${tests_dir}/test_encode.cpp"
        DEPENDENCIES cy::ui-render)

    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    set(pipeline_tests "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests")
    set(rts_sample "${CMAKE_SOURCE_DIR}/samples/13-rts-selection")
    cy_add_test(
        NAME ui
        KIND render
        SOURCES
            "${tests_dir}/test_ui_device.cpp"
            "${pipeline_tests}/frame_scene.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
            "${rts_sample}/hud.cpp"
        DEPENDENCIES cy::ui-render cy::ui-text cy::ui-console cy::rendering-pipeline
                     cy::rendering-particles cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_ui
                               PRIVATE "${tests_dir}" "${pipeline_tests}"
                                       "${CMAKE_SOURCE_DIR}/tests/render" "${rts_sample}")
    target_compile_definitions(cy_test_render_ui
                               PRIVATE CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
                                       CY_UI_RENDER_TEST_DIR="${tests_dir}")
    set_tests_properties(render.ui PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_ui_render_declare_tests)
