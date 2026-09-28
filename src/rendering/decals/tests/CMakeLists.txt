# src/rendering/decals/tests/ — decals in the frame, with and without a device.
#
#   rendering_decals  KIND unit. The table's layout word for word: the header, a record's fields
#                     relative to the origin in double precision, the lists copied out of an
#                     assignment, and the two refusals. No device.
#
#   decals            KIND render, Vulkan only. The pipeline suites' scene with a directional shadow
#                     map and one box on the floor: no table is byte-identical to the committed
#                     pre-change reference in references/, a decal layer of none is byte-identical
#                     to no decal, a decal changes only pixels inside its box, is lit and shadowed
#                     as its receiver is, fades on a steep receiver, applies in its own order, is
#                     found through the cluster lists without losing one, costs no draw call at
#                     twenty thousand, and tilts the normal by its relief. It compiles
#                     src/rendering/pipeline/tests/frame_scene.cpp — the one scene — rather than a
#                     second, and tests/render/golden.cpp for the PNG encoder.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory for the reason
# src/rendering/pipeline/tests/CMakeLists.txt gives.
set_property(GLOBAL PROPERTY CY_DECALS_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_decals_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_DECALS_TESTS_DIR)
    foreach(kind IN ITEMS unit render)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/decals/tests: the '${kind}' entry of the test taxonomy could not be "
                "read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME rendering_decals
        KIND unit
        SOURCES "${tests_dir}/test_decal_table.cpp"
        DEPENDENCIES cy::rendering-decals)

    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    set(pipeline_tests "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests")
    cy_add_test(
        NAME decals
        KIND render
        SOURCES
            "${tests_dir}/test_decals_device.cpp"
            "${pipeline_tests}/frame_scene.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-decals cy::rendering-lighting cy::rendering-pipeline
                     cy::rendering-particles cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_decals
                               PRIVATE "${tests_dir}" "${pipeline_tests}"
                                       "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_test_render_decals
                               PRIVATE CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
                                       CY_DECALS_TEST_DIR="${tests_dir}")
    set_tests_properties(render.decals PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_decals_declare_tests)
