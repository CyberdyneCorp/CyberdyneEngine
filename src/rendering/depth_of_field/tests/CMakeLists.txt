# src/rendering/depth_of_field/tests/ — depth of field, on the host and on a device.
#
#   render_dof        KIND unit. The host twins against `circle_of_confusion` and the projection:
#                     the radius in pixels at every distance, the depth's inverse for the finite, the
#                     infinite and the orthographic matrix, the pinhole's zero, the aperture's
#                     polygon and the gather's taps. No device.
#
#   depth_of_field    KIND render, Vulkan only. The five dispatches over synthetic colour and depth —
#                     the in-focus plane unchanged, a far edge blurred by the radius the lens gives,
#                     the near field over a focused edge and the far field kept off a nearer one, a
#                     polygonal aperture — and then `pipeline_test::FrameScene`, the one scene the
#                     pipeline suites render, with the stage in its chain: a pinhole identical to the
#                     frame without it, and the frame without it against a reference drawn with the
#                     shaders from before this module; and, on the null backend's command log,
#                     that no draw after the stage's dispatches has its sets bound for compute.
#                     It compiles
#                     src/rendering/pipeline/tests/frame_scene.cpp and tests/render/golden.cpp.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory for the reason
# src/rendering/pipeline/tests/CMakeLists.txt gives.
set_property(GLOBAL PROPERTY CY_DOF_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_dof_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_DOF_TESTS_DIR)
    foreach(kind IN ITEMS unit render)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/depth_of_field/tests: the '${kind}' entry of the test taxonomy "
                "could not be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME render_dof
        KIND unit
        SOURCES "${tests_dir}/test_focus.cpp"
        DEPENDENCIES cy::rendering-depth-of-field)

    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    set(pipeline_tests "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests")
    cy_add_test(
        NAME depth_of_field
        KIND render
        SOURCES
            "${tests_dir}/test_dof_device.cpp"
            "${pipeline_tests}/frame_scene.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-depth-of-field cy::rendering-pipeline cy::rendering-particles
                     cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_depth_of_field
                               PRIVATE "${tests_dir}" "${pipeline_tests}"
                                       "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_test_render_depth_of_field
                               PRIVATE CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
                                       CY_DOF_TEST_DIR="${tests_dir}")
    set_tests_properties(render.depth_of_field PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_dof_declare_tests)
