# src/rendering/light_probes/tests/ — an irradiance volume in the frame, on a device.
#
#   light_probes    KIND render, Vulkan only. The pipeline suites' scene rearranged into a corner
#                   with a red wall, and a volume captured from the same boxes: colour bleeding near
#                   the red wall and not far from it, an unlit wall lit by bounce where the flat
#                   ambient is uniform, the frame's term against the host volume's, off identical
#                   to absent and absent identical to the committed pre-change reference in
#                   references/, and ambient occlusion multiplying the volume's term. It compiles
#                   src/rendering/pipeline/tests/frame_scene.cpp — the one scene — rather than a
#                   second, and tests/render/golden.cpp for the PNG encoder.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory for the reason
# src/rendering/pipeline/tests/CMakeLists.txt gives.
set_property(GLOBAL PROPERTY CY_LIGHT_PROBES_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_light_probes_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_LIGHT_PROBES_TESTS_DIR)
    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_render)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_render)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/rendering/light_probes/tests: the 'render' entry of the test taxonomy could not "
            "be read from tests/CMakeLists.txt.")
    endif()
    set(CY_TEST_BUDGET_render "${budget}")
    set(CY_TEST_TIMEOUT_render "${timeout}")

    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    set(pipeline_tests "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests")
    cy_add_test(
        NAME light_probes
        KIND render
        SOURCES
            "${tests_dir}/test_light_probes_device.cpp"
            "${pipeline_tests}/frame_scene.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-light-probes cy::rendering-gi cy::rendering-pipeline
                     cy::rendering-occlusion cy::rendering-particles cy::rhi-null
                     cy::rhi-vulkan)
    target_include_directories(cy_test_render_light_probes
                               PRIVATE "${tests_dir}" "${pipeline_tests}"
                                       "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_test_render_light_probes
                               PRIVATE CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
                                       CY_LIGHT_PROBES_TEST_DIR="${tests_dir}")
    set_tests_properties(render.light_probes PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_light_probes_declare_tests)
