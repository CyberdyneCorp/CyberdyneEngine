# src/rendering/grading/tests/ — exposure and colour grading on a device.
#
#   grading   KIND render, Vulkan only. The metering dispatches against their host twins bin for bin,
#             then `pipeline_test::FrameScene` — the one scene the pipeline suites render — with the
#             graded resolve as its post-process: auto-exposure's convergence and step response, a
#             manual EV's factor, identity and neutral grades byte-identical to the ungraded frame, a
#             known LUT's per-pixel result, the committed looks, and the frame without grading
#             against a reference drawn before this module existed. It compiles
#             src/rendering/pipeline/tests/frame_scene.cpp — the one scene — and tests/render/golden.cpp
#             for the PNG encoder.
#
# The host arithmetic under every one of these is `unit.render_post` and `integration.render_post_bake`.
# Declared with cy_add_test(), deferred to the end of the top-level directory for the reason
# src/rendering/pipeline/tests/CMakeLists.txt gives.
set_property(GLOBAL PROPERTY CY_GRADING_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_grading_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_GRADING_TESTS_DIR)
    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_render)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_render)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/rendering/grading/tests: the 'render' entry of the test taxonomy could not be read "
            "from tests/CMakeLists.txt.")
    endif()
    set(CY_TEST_BUDGET_render "${budget}")
    set(CY_TEST_TIMEOUT_render "${timeout}")

    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    set(pipeline_tests "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests")
    cy_add_test(
        NAME grading
        KIND render
        SOURCES
            "${tests_dir}/test_grading_device.cpp"
            "${tests_dir}/test_metering_device.cpp"
            "${pipeline_tests}/frame_scene.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-grading cy::rendering-pipeline cy::rendering-particles
                     cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_grading
                               PRIVATE "${tests_dir}" "${pipeline_tests}"
                                       "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_test_render_grading
                               PRIVATE CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
                                       CY_GRADING_TEST_DIR="${tests_dir}"
                                       CY_REPOSITORY_DIR="${CMAKE_SOURCE_DIR}")
    set_tests_properties(render.grading PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_grading_declare_tests)
