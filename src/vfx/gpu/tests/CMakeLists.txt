# src/vfx/gpu/tests/ — the VFX compute dispatch's suite. M10 task 5.1 and 5.2.
#
# KIND render, and declared only when CY_RENDERER_VULKAN is on — for the reason tests/render/ gives:
# a suite that always skips is a suite whose failure nobody would notice. With the backend compiled
# in and no GPU it SKIPS LOUDLY, naming the backend that was selected instead.
#
# It compiles src/vfx/tests/effects.cpp so that the effect it runs is the SAME spark plume
# `integration.vfx_compiler`, `integration.vfx` and `render.vfx` run. A second fixture would drift
# from the first inside a milestone and then two suites would disagree about a defect neither of
# them can see; tests/render/README.md records that as the rule.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing, exactly as src/vfx/tests/ does it
# and for the same reason.
set_property(GLOBAL PROPERTY CY_VFX_GPU_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set_property(GLOBAL PROPERTY CY_VFX_GPU_EFFECTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../tests")

function(cy_vfx_gpu_declare_tests)
    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    get_property(tests_dir GLOBAL PROPERTY CY_VFX_GPU_TESTS_DIR)
    get_property(effects_dir GLOBAL PROPERTY CY_VFX_GPU_EFFECTS_DIR)

    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_render)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_render)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/vfx/gpu/tests: the 'render' entry of the test taxonomy could not be read from "
            "tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
            "deferral around it should go away — declare the suite from tests/render/ instead.")
    endif()
    set(CY_TEST_BUDGET_render "${budget}")
    set(CY_TEST_TIMEOUT_render "${timeout}")

    cy_add_test(
        NAME vfx_gpu
        KIND render
        SOURCES
            "${effects_dir}/effects.cpp"
            "${tests_dir}/test_vfx_gpu_pass.cpp"
        DEPENDENCIES cy::vfx cy::vfx-gpu cy::rendering-graph cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_vfx_gpu PRIVATE "${tests_dir}" "${effects_dir}")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_vfx_gpu_declare_tests)
