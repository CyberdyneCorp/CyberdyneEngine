# src/rendering/virtual_texturing/tests/ — CyberTexture on a device. M7 tasks 4.1, 4.2 and 4.3.
#
#   render.virtual_texturing_gpu   test_vt_frame.cpp: the feedback buffer a shader writes, resolved
#                                  on the GPU, with what the CPU actually mapped measured against
#                                  the pixel count (4.1); the page table a shader samples, over the
#                                  whole address space, checked against `VirtualTextureSystem::
#                                  sample()` both before and after the mip tail is resident (4.2).
#                                  test_frame_residency.cpp: two subsystems arbitrating for one
#                                  budget, fed by the same frame's two dispatches (4.3).
#
# KIND render, not unit: these need a Vulkan device, and the render taxonomy is the one whose budget
# and timeout are written for a suite that talks to a GPU. The CPU-side suites for this capability
# are still `unit.virtual_texturing` in src/servers/render/virtual_texturing/tests/ and are not
# replaced by these — the two answer different questions, and the whole design argument of the layer
# 2 module is that its questions can be answered on a machine with no GPU.
#
# DECLARED ONLY WHEN CY_RENDERER_VULKAN IS ON, for the reason tests/render/CMakeLists.txt gives: a
# suite that always skips is a suite whose failure nobody would notice. With the backend compiled in
# and no GPU on the machine it SKIPS LOUDLY, naming the backend that was selected instead.
#
# IT LINKS cy::rendering-gpu-culling, and that is not incidental. Task 4.3's second claimant is the
# level each instance was actually drawn at, which only the culling dispatch knows — a residency test
# that invented its geometry feedback would be the single-subsystem test with a costume on.
set_property(GLOBAL PROPERTY CY_VT_GPU_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_virtual_texturing_gpu_declare_tests)
    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    get_property(cy_vt_tests_dir GLOBAL PROPERTY CY_VT_GPU_TESTS_DIR)

    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_render)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_render)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/rendering/virtual_texturing/tests: the 'render' entry of the test taxonomy could "
            "not be read from tests/CMakeLists.txt. If the budget variables were renamed, this "
            "lookup and the deferral around it should go away — declare the suite from "
            "tests/render/ instead.")
    endif()
    set(CY_TEST_BUDGET_render "${budget}")
    set(CY_TEST_TIMEOUT_render "${timeout}")

    cy_add_test(
        NAME virtual_texturing_gpu
        KIND render
        SOURCES
            "${cy_vt_tests_dir}/test_vt_frame.cpp"
            "${cy_vt_tests_dir}/test_frame_residency.cpp"
        DEPENDENCIES cy::rendering-gpu-culling cy::rendering-graph cy::rendering-virtual-texturing
                     cy::rhi cy::rhi-null cy::rhi-vulkan cy::servers-render-virtual-texturing
                     cy::servers-residency)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_virtual_texturing_gpu_declare_tests)
