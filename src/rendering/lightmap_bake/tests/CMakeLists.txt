# src/rendering/lightmap_bake/tests/ — the lightmap bake, headless.
#
#   render_lightmap_atlas   the packer and the address word: shared pages, per-object resolution,
#                           the block grid and the gutter, and the address round trip the frame
#                           decodes. Arithmetic, so the unit budget.
#   render_lightmap_mips    the mip chain over an atlas drawn by hand: every protected level reads
#                           only its own chart where a plain box filter bleeds. Arithmetic,
#                           but over a whole 512 page, which is past the unit budget.
#   render_lightmap_bake    the bake over real rooms: atlas texels against the path tracer's ground
#                           truth, directional and SH L1 keeping a normal map's response where
#                           irradiance-only does not, seams, emissive light, alpha-tested and
#                           transparent occlusion, the caches' seeds from the same run, and the
#                           cooked payload. Every case traces real rays, which is an integration
#                           test.
#
# Declared through the deferral src/rendering/gi/tests/ uses, for the reason it gives.
set_property(GLOBAL PROPERTY CY_LIGHTMAP_BAKE_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_lightmap_bake_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_LIGHTMAP_BAKE_TESTS_DIR)
    foreach(kind unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/lightmap_bake/tests: the '${kind}' entry of the test taxonomy "
                "could not be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME render_lightmap_atlas
        KIND unit
        SOURCES "${tests_dir}/test_atlas.cpp"
        DEPENDENCIES cy::rendering-lightmap-bake)

    cy_add_test(
        NAME render_lightmap_mips
        KIND integration
        SOURCES "${tests_dir}/test_mips.cpp"
        DEPENDENCIES cy::rendering-lightmap-bake)

    cy_add_test(
        NAME render_lightmap_bake
        KIND integration
        SOURCES "${tests_dir}/test_bake.cpp"
        DEPENDENCIES cy::rendering-lightmap-bake cy::rendering-gi)
    target_include_directories(cy_test_integration_render_lightmap_bake
                               PRIVATE "${CMAKE_SOURCE_DIR}/src/rendering/gi/tests")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_lightmap_bake_declare_tests)
