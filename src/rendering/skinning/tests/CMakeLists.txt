# src/rendering/skinning/tests/ — the skinning dispatch, on a device. M8.d.
#
#   render.skinning      the dispatch against `cpu_reference_skin`, BY COMPARING BUFFERS: one arm
#                        skinned twice, positions and re-encoded normal-tangent frames compared
#                        vertex by vertex, plus the pose offset, double buffering, eight influences
#                        and the refusals.
#   render.skinned_draw  those same output buffers BOUND AS A VERTEX BUFFER and rasterised, with the
#                        covered pixels checked against addresses computed from the pose. This is
#                        the case that makes "a skinned mesh is drawn" a measured claim.
#
# KIND render, not unit: both need a Vulkan device, and the render taxonomy is the one whose budget
# and timeout are written for a suite that talks to a GPU. The arithmetic they are checked against
# is in src/servers/render/geometry/tests/test_skin_dispatch.cpp, which is a unit suite and runs on
# a machine with no GPU at all — so the chain paper → reference → dispatch has its first two links
# gated everywhere and only the last one needs hardware.
#
# DECLARED ONLY WHEN CY_RENDERER_VULKAN IS ON, for the reason tests/render/CMakeLists.txt gives: a
# suite that always skips is a suite whose failure nobody would notice. With the backend compiled in
# and no GPU on the machine they SKIP LOUDLY, naming the backend that was selected instead.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing, exactly as
# src/rendering/gpu_culling/tests/CMakeLists.txt does it.
set_property(GLOBAL PROPERTY CY_SKINNING_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_skinning_declare_tests)
    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    get_property(cy_skinning_tests_dir GLOBAL PROPERTY CY_SKINNING_TESTS_DIR)

    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_render)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_render)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/rendering/skinning/tests: the 'render' entry of the test taxonomy could not be "
            "read from tests/CMakeLists.txt.")
    endif()
    set(CY_TEST_BUDGET_render "${budget}")
    set(CY_TEST_TIMEOUT_render "${timeout}")

    cy_add_test(
        NAME skinning
        KIND render
        SOURCES "${cy_skinning_tests_dir}/test_skin_pass.cpp"
        DEPENDENCIES cy::rendering-graph cy::rendering-skinning cy::rhi cy::rhi-null cy::rhi-vulkan
                     cy::servers-render-geometry)
    target_include_directories(cy_test_render_skinning PRIVATE "${cy_skinning_tests_dir}")

    # The drawn case is its own suite rather than another case in the first, because it needs a
    # graphics pipeline and the conventions suite's compiled shaders — and because "the dispatch
    # computes the right vertices" and "something drew them" are different claims that should fail
    # separately.
    cy_add_test(
        NAME skinned_draw
        KIND render
        SOURCES "${cy_skinning_tests_dir}/test_skin_draw.cpp"
        DEPENDENCIES cy::rendering-graph cy::rendering-skinning cy::rhi cy::rhi-null cy::rhi-vulkan
                     cy::servers-render-geometry)
    # The suite includes "skin_fixture.h" and tests/render/shaders/conventions_spirv.h by relative
    # path: the sources are compiled from the top-level binary directory, so a relative include
    # finds nothing without both directories on the path. The shader pair is REUSED rather than
    # rewritten, because a second vertex stage would be a second place for the clip convention to
    # drift — which is the same argument tests/render/CMakeLists.txt makes about its own scene.
    target_include_directories(cy_test_render_skinned_draw
                               PRIVATE "${cy_skinning_tests_dir}" "${CMAKE_SOURCE_DIR}/tests/render")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_skinning_declare_tests)
