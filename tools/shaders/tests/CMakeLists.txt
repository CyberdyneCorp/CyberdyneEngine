# tools/shaders/tests/ — the shader target tool's suite. M11.d, closing
# `m11c:every-shader-reaches-every-target`.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* tools/, so that function does not exist while this file is read. The
# declaration is therefore deferred to the end of the top-level directory's processing, and the
# taxonomy is read back out of tests/ rather than restated; tools/material/tests/ does the same.
#
# SMOKE, because the case loads a real toolchain — Slang, and DXC when this build emits DXIL — and
# compiles the engine's own shaders for every target. That is the taxonomy's bucket for a test whose
# cost is a real toolchain, as `smoke.shader_slang` is.
set_property(GLOBAL PROPERTY CY_SHADER_TARGETS_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_shader_targets_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_SHADER_TARGETS_TESTS_DIR)

    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_smoke)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_smoke)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "tools/shaders/tests: the 'smoke' entry of the test taxonomy could not be read from "
            "tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
            "deferral around it should go away — declare the suite from tests/smoke/ instead.")
    endif()
    set(CY_TEST_BUDGET_smoke "${budget}")
    set(CY_TEST_TIMEOUT_smoke "${timeout}")

    # HOW MANY TARGETS THIS BUILD MUST EMIT, so a run that silently lost DXC — the library is placed
    # beside libslang by `cy_place_dxc`, and a build of this suite alone would not otherwise run it
    # — cannot pass by comparing two targets and calling that every target.
    if(CY_SHADER_DXIL)
        set(expected_targets 3)
    else()
        set(expected_targets 2)
    endif()
    cy_add_test(
        NAME shader_targets
        KIND smoke
        SOURCES "${tests_dir}/test_shader_targets.cpp"
        DEPENDENCIES cy::shader-targets
        DEFINITIONS CY_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
                    CY_SHADER_TARGETS_EXPECTED=${expected_targets})
    if(TARGET cy_place_dxc)
        add_dependencies(cy_test_smoke_shader_targets cy_place_dxc)
    endif()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_shader_targets_declare_tests)
