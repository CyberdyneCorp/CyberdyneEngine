# src/rendering/selection/tests/ — selection outlines on the host, and on a device.
#
#   rendering_selection     KIND unit. The marks, the palette and the edge pass's host reference
#                           over small synthetic masks whose answers can be written down: a selected
#                           ring is every pixel within its width outside the silhouette, a hovered
#                           glow falls off, the occluded style is drawn only beside a hidden part
#                           and the fill only on it, and a tie goes to the first pixel in scan
#                           order. The embedded mask module's reads of the frame's blocks, against
#                           the C++ offsets. And the gameplay half: the `SelectionHighlight`
#                           component gathered into marks keyed by entity. No device.
#                           (`unit.abi_selection`, in src/abi/tests/, drives the same component
#                           through `cy_get_interface` as a Swift module does.)
#
#   selection_outlines      KIND render, Vulkan only. `pipeline_test::FrameScene` arranged as a small
#                           strategy-game field — units, one partly behind a building — outlined
#                           through the frame's own `SelectionOutlines` stage: outlines exactly on
#                           the marked silhouettes and within their width, unmarked objects
#                           untouched, colour and width following the API, the occluded style only
#                           where a unit is hidden, and nothing marked byte-identical to a committed
#                           reference drawn before the stage existed. It compiles
#                           src/rendering/pipeline/tests/frame_scene.cpp — the one scene — and
#                           tests/render/golden.cpp for the PNG encoder.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory for the reason
# src/rendering/pipeline/tests/CMakeLists.txt gives.
set_property(GLOBAL PROPERTY CY_SELECTION_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_selection_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_SELECTION_TESTS_DIR)
    foreach(kind IN ITEMS unit render)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/selection/tests: the '${kind}' entry of the test taxonomy could not "
                "be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME rendering_selection
        KIND unit
        SOURCES "${tests_dir}/test_highlight.cpp" "${tests_dir}/test_component.cpp"
                "${tests_dir}/test_modules.cpp"
        DEPENDENCIES cy::rendering-selection)
    # The embedded modules, for the layout check in test_modules.cpp.
    target_include_directories(cy_test_unit_rendering_selection PRIVATE "${tests_dir}/../src")

    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    set(pipeline_tests "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests")
    cy_add_test(
        NAME selection_outlines
        KIND render
        SOURCES
            "${tests_dir}/test_outlines_device.cpp"
            "${pipeline_tests}/frame_scene.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-selection cy::rendering-pipeline cy::rendering-particles
                     cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_selection_outlines
                               PRIVATE "${tests_dir}" "${pipeline_tests}"
                                       "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_test_render_selection_outlines
                               PRIVATE CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
                                       CY_SELECTION_TEST_DIR="${tests_dir}")
    set_tests_properties(render.selection_outlines PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_selection_declare_tests)
