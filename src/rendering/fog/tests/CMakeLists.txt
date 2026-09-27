# src/rendering/fog/tests/ — volumetric fog on the host, and on a device.
#
#   rendering_fog     KIND unit. The medium (Koschmieder's visibility, the height fog's profile,
#                     every volume shape, a mixture's phase), the constants' refusals, the header
#                     against the aerial perspective table's, the march against the single-
#                     scattering integral and a closed form, the empty medium's exact identity, the
#                     lookup against the march, and the shadow lookup. No device.
#
#   rendering_fog_air KIND integration. The march composited with the atmosphere's own froxel
#                     table: an empty fog reproduces the table, a switched-off table leaves the fog
#                     as it was, and together they transmit the product and add less than the sum.
#                     Integration because the atmosphere's tables are built once, a few hundred ms.
#
#   volumetric_fog    KIND render, Vulkan only. The frame the pipeline suites render, given a
#                     directional shadow map and the fog stage: off identical to a committed
#                     reference drawn by the frame shader before this change, an empty medium
#                     identical to off, a shaft of shadowed froxels behind an occluder, the device's
#                     volume against the single-scattering integral and against the host march, and
#                     surfaces attenuated with their distance, and the atmosphere-table variant
#                     against the host march over a table the processor built. It compiles
#                     src/rendering/pipeline/tests/frame_scene.cpp — the one scene — and
#                     tests/render/golden.cpp for the PNG encoder.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory for the reason
# src/rendering/pipeline/tests/CMakeLists.txt gives.
set_property(GLOBAL PROPERTY CY_FOG_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_fog_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_FOG_TESTS_DIR)
    foreach(kind IN ITEMS unit integration render)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/fog/tests: the '${kind}' entry of the test taxonomy could not be "
                "read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME rendering_fog
        KIND unit
        SOURCES "${tests_dir}/test_fog.cpp"
        DEPENDENCIES cy::rendering-fog cy::rendering-sky)
    cy_add_test(
        NAME rendering_fog_air
        KIND integration
        SOURCES "${tests_dir}/test_fog_air.cpp"
        DEPENDENCIES cy::rendering-fog cy::rendering-sky)

    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    set(pipeline_tests "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests")
    cy_add_test(
        NAME volumetric_fog
        KIND render
        SOURCES
            "${tests_dir}/test_fog_device.cpp"
            "${pipeline_tests}/frame_scene.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-fog cy::rendering-pipeline cy::rendering-particles cy::rhi-null
                     cy::rhi-vulkan)
    target_include_directories(cy_test_render_volumetric_fog
                               PRIVATE "${tests_dir}" "${pipeline_tests}"
                                       "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_test_render_volumetric_fog
                               PRIVATE CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
                                       CY_FOG_TEST_DIR="${tests_dir}")
    set_tests_properties(render.volumetric_fog PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_fog_declare_tests)
