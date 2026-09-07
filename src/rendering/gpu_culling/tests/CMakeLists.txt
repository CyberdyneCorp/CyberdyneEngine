# src/rendering/gpu_culling/tests/ — the culling dispatch, on a device. M7 task 5.1.
#
#   render.gpu_culling   two files. test_gpu_cull_pass.cpp culls one scene twice — once through
#                        `cpu_reference_cull` and once through the compute dispatch — and compares
#                        the five output buffers word for word (task 5.1).
#                        test_renderer_consumes.cpp runs the whole renderer path: a SpatialIndex is
#                        published into the GPU scene, dispatched, and turned back into the
#                        `CullResults` cy::rendering-forward consumes — then compared against what
#                        `cull_view()` and `select_lods()` produce for the same index (task 5.2).
#
# KIND render, not unit: it needs a Vulkan device, and the render taxonomy is the one whose budget
# and timeout are written for a suite that talks to a GPU.
#
# DECLARED ONLY WHEN CY_RENDERER_VULKAN IS ON, for the reason tests/render/CMakeLists.txt gives: a
# suite that always skips is a suite whose failure nobody would notice. With the backend compiled in
# and no GPU on the machine it SKIPS LOUDLY, naming the backend that was selected instead.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing, exactly as
# src/rendering/culling/tests/CMakeLists.txt does it.
set_property(GLOBAL PROPERTY CY_GPU_CULLING_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_gpu_culling_declare_tests)
    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    get_property(cy_gpu_culling_tests_dir GLOBAL PROPERTY CY_GPU_CULLING_TESTS_DIR)

    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_render)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_render)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/rendering/gpu_culling/tests: the 'render' entry of the test taxonomy could not be "
            "read from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
            "the deferral around it should go away — declare the suite from tests/render/ instead.")
    endif()
    set(CY_TEST_BUDGET_render "${budget}")
    set(CY_TEST_TIMEOUT_render "${timeout}")

    cy_add_test(
        NAME gpu_culling
        KIND render
        SOURCES
            "${cy_gpu_culling_tests_dir}/test_gpu_cull_pass.cpp"
            "${cy_gpu_culling_tests_dir}/test_renderer_consumes.cpp"
        DEPENDENCIES cy::rendering-culling cy::rendering-gpu-culling cy::rendering-graph cy::rhi
                     cy::rhi-null cy::rhi-vulkan cy::servers-render-culling)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_gpu_culling_declare_tests)
