# src/backends/shader/tests/ — the shader system's suites. Section 3 of the M3 tasks.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred to the end of the top-level directory's processing, by which time the
# taxonomy has been declared; the taxonomy itself is read back out of tests/ rather than restated,
# because a budget copied into a second file is a budget that drifts. src/backends/rhi/, src/ecs/,
# src/scene/ and src/runtime/ all do the same, for the same reason.
#
#   shader           the arithmetic at unit cost: SPIR-V reflection over checked-in modules, the
#                    permutation key, the diagnostic parser, the global block's std140 layout, the
#                    cache key, the library encoding and the pipeline state key. No I/O, no threads.
#   shader_slang     the Slang front end itself, when CY_SHADER_SLANG is on. Smoke, because
#                    creating a Slang global session loads its core module and costs a second before
#                    anything is compiled.
#   shader_pipeline  the source registry, the library encoding and hot reload: they mount a
#                    namespace, read files
#                    through it and step a clock across the watcher's settle period. In-memory, but
#                    it is I/O through the same interface a directory mount uses, and the reload
#                    cases reflect several modules each — integration by cost, not by subject.
#
# NEITHER SUITE NEEDS A SHADER COMPILER, which is the point: they run in the default configuration,
# with CY_SHADER_SLANG off. The modules they reflect were compiled once by Slang and checked in
# (fixtures/spirv_probe.h), and the front end they compile through is the SPIR-V passthrough — which
# is the shipping path rather than a stub.
set_property(GLOBAL PROPERTY CY_SHADER_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_shader_declare_tests)
    get_property(cy_shader_tests_dir GLOBAL PROPERTY CY_SHADER_TESTS_DIR)

    foreach(kind IN ITEMS unit integration smoke)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/backends/shader/tests: the '${kind}' entry of the test taxonomy could not be "
                "read from tests/CMakeLists.txt. If the budget variables were renamed, this lookup "
                "and the deferral around it should go away — declare the suites from tests/unit/ "
                "instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    # Sources are absolute, because the deferred call runs in the top-level directory's scope and a
    # relative path would be resolved against that directory rather than against this one.
    cy_add_test(
        NAME shader
        KIND unit
        SOURCES
            "${cy_shader_tests_dir}/test_cache.cpp"
            "${cy_shader_tests_dir}/test_diagnostics.cpp"
            "${cy_shader_tests_dir}/test_globals.cpp"
            "${cy_shader_tests_dir}/test_library.cpp"
            "${cy_shader_tests_dir}/test_permutation.cpp"
            "${cy_shader_tests_dir}/test_pipeline.cpp"
            "${cy_shader_tests_dir}/test_reflection.cpp"
        DEPENDENCIES cy::shader)

    cy_add_test(
        NAME shader_pipeline
        KIND integration
        SOURCES
            "${cy_shader_tests_dir}/test_hot_reload.cpp"
            "${cy_shader_tests_dir}/test_library_encoding.cpp"
            "${cy_shader_tests_dir}/test_source.cpp"
        DEPENDENCIES cy::shader)

    # The Slang front end's own suite. Declared only when the front end is built, because it is the
    # only configuration in which it exists: `shader-system` requires a shipping build to contain no
    # Slang compiler, and cy::shader-slang is excluded from the link rather than stubbed out.
    #
    # SMOKE, AND THE NUMBER IS WHY. Creating a Slang global session loads the compiler's core
    # module, which is measured at about 1.1 seconds on this machine — past the integration budget
    # on its own, before any shader is compiled. A session could be shared across the suite to hide
    # that, and it would be the wrong trade: the cases would then depend on each other's order for
    # no gain, and the taxonomy already has a bucket for a test whose cost is a real toolchain.
    if(CY_SHADER_SLANG)
        cy_add_test(
            NAME shader_slang
            KIND smoke
            SOURCES "${cy_shader_tests_dir}/test_slang_compiler.cpp"
            DEPENDENCIES cy::shader cy::shader-slang)
    endif()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_shader_declare_tests)
