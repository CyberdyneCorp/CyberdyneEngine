# src/vfx/tests/ — CyberVFX's suites. M8.c section 2.
#
# THREE SUITES, AND THE SPLIT IS THE TAXONOMY'S.
#
#   vfx_values      KIND unit. Precision selection, the storage conversions, the budget controller's
#                   arithmetic and the event channels' two bounds. Pure value work: no world, no
#                   device, no graph, no particle system — the four things `testing-and-quality`
#                   names as what does NOT fit a millisecond, and which this milestone's brief names
#                   again. Nothing in this suite allocates a pool.
#
#   vfx_compiler    KIND integration. Every case authors several stage graphs, compiles them through
#                   the shared expression core to a fixed point and emits their Slang.
#                   `src/graph/tests/` records that exactly this shape does not fit the unit tier in
#                   a Debug configuration, and the subject IS the repeated compilation.
#
#   vfx             KIND integration. A simulation world, a shared pool, four hundred instances and
#                   a synthetic overload, plus the six renderer kinds' publications (M10 task 5.3) —
#                   which need a world and a cooked effect for the same reason and so are the same
#                   suite rather than a fourth one.
#
# THE DEVICE SUITE IS DECLARED SEPARATELY, and only when CY_RENDERER_VULKAN is on — for the reason
# tests/render/ gives: a suite that always skips is a suite whose failure nobody would notice. With
# the backend compiled in and no GPU it SKIPS LOUDLY. It compiles tests/render/golden.cpp so that
# the pictures it writes go through the same PNG encoder the golden suite already uses; a second
# encoder in the tree is a second thing to be wrong about what the engine writes.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing, exactly as src/graph/tests/ and
# src/rendering/pipeline/tests/ do it.
set_property(GLOBAL PROPERTY CY_VFX_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_vfx_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_VFX_TESTS_DIR)

    foreach(kind IN ITEMS unit integration render)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/vfx/tests: the '${kind}' entry of the test taxonomy could not be read from "
                "tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
                "deferral around it should go away — declare the suites from tests/unit/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME vfx_values
        KIND unit
        SOURCES "${tests_dir}/test_vfx_unit.cpp"
        DEPENDENCIES cy::vfx)

    cy_add_test(
        NAME vfx_compiler
        KIND integration
        SOURCES
            "${tests_dir}/effects.cpp"
            "${tests_dir}/test_vfx_compiler.cpp"
        DEPENDENCIES cy::vfx)
    target_include_directories(cy_test_integration_vfx_compiler PRIVATE "${tests_dir}")

    cy_add_test(
        NAME vfx
        KIND integration
        SOURCES
            "${tests_dir}/effects.cpp"
            "${tests_dir}/test_vfx_runtime.cpp"
            "${tests_dir}/test_vfx_renderers.cpp"
        DEPENDENCIES cy::vfx)
    target_include_directories(cy_test_integration_vfx PRIVATE "${tests_dir}")

    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    cy_add_test(
        NAME vfx
        KIND render
        SOURCES
            "${tests_dir}/effects.cpp"
            "${tests_dir}/vfx_scene.cpp"
            "${tests_dir}/test_vfx_device.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::vfx cy::rendering-assembly cy::rendering-graph cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_vfx
                               PRIVATE "${tests_dir}" "${CMAKE_SOURCE_DIR}/tests/render")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_vfx_declare_tests)
