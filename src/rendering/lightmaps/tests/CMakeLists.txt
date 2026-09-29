# src/rendering/lightmaps/tests/ — a baked lightmap in the frame, on a device.
#
#   lightmaps       KIND render, on Vulkan, and lightmaps_metal on Metal. `render.light_probes`' corner — the pipeline suites'
#                   scene with a red wall, a back wall and two cubes — baked by
#                   lightmap_bake::bake_lightmaps from the same boxes: the red wall bleeding onto
#                   what is near it, the frame's term against the host's `sample_lightmap` pixel for
#                   pixel, the directional and SH L1 encodings at the geometric normal, the volume
#                   and the lightmap combined as gi::exclusion_for() says, and no lightmap bound
#                   byte-identical to the committed reference the frame shader drew before lightmaps
#                   existed. It compiles src/rendering/pipeline/tests/frame_scene.cpp — the one
#                   scene — and tests/render/golden.cpp for the PNG encoder.
#
#   lightmap_frame  KIND unit: the ambient-source rule and the view-block words, no device.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory for the reason
# src/rendering/pipeline/tests/CMakeLists.txt gives.
set_property(GLOBAL PROPERTY CY_LIGHTMAPS_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_lightmaps_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_LIGHTMAPS_TESTS_DIR)
    foreach(kind unit render)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/lightmaps/tests: the '${kind}' entry of the test taxonomy could "
                "not be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME lightmap_frame
        KIND unit
        SOURCES "${tests_dir}/test_lightmap_frame.cpp"
        DEPENDENCIES cy::rendering-lightmaps)

    # ONE SOURCE, ONE LEG PER BACKEND, as src/rendering/pipeline/tests/ declares its device suite:
    # `lightmaps` on Vulkan and `lightmaps_metal` on Metal (plain `lightmaps` in a Metal-only tree).
    function(cy_lightmaps_device_suite name backend define)
        set(pipeline_tests "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests")
        cy_add_test(
            NAME ${name}
            KIND render
            SOURCES
                "${tests_dir}/test_lightmaps_device.cpp"
                "${pipeline_tests}/frame_scene.cpp"
                "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
            DEPENDENCIES cy::rendering-lightmaps cy::rendering-lightmap-bake
                         cy::rendering-light-probes cy::rendering-gi cy::rendering-pipeline
                         cy::rendering-particles cy::rhi-null ${backend})
        set(target "cy_test_render_${name}")
        target_include_directories(${target} PRIVATE "${tests_dir}" "${pipeline_tests}"
                                                     "${CMAKE_SOURCE_DIR}/tests/render")
        target_compile_definitions(${target}
                                   PRIVATE ${define} CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
                                           CY_LIGHTMAPS_TEST_DIR="${tests_dir}")
        set_tests_properties(render.${name} PROPERTIES
                             SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
    endfunction()

    if(CY_RENDERER_VULKAN)
        cy_lightmaps_device_suite(lightmaps cy::rhi-vulkan CY_TEST_LIGHTMAPS_VULKAN=1)
    endif()
    if(APPLE AND CY_RENDERER_METAL)
        if(CY_RENDERER_VULKAN)
            cy_lightmaps_device_suite(lightmaps_metal cy::rhi-metal CY_TEST_LIGHTMAPS_METAL=1)
        else()
            cy_lightmaps_device_suite(lightmaps cy::rhi-metal CY_TEST_LIGHTMAPS_METAL=1)
        endif()
    endif()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_lightmaps_declare_tests)
