# src/rendering/viewmodel/tests/ — the first-person weapon view, on the host and on a device.
#
#   render_viewmodel   KIND unit. The squeezed projection and what it guarantees, the two views one
#                      camera produces, last frame's lens, and the view-block words. No device.
#
#   viewmodel_assembly KIND integration. The assembly's second cull, the weapon layers kept out of
#                      the world's, and the mark and shadow policy the weapon view's draws carry.
#                      No device; a whole `FrameAssembly` a case.
#
#   weapon_view        KIND render, Vulkan only. `pipeline_test::FrameScene` with a first-person
#                      weapon on a layer of its own: off against the frame before, a weapon pushed
#                      into a wall, its own near plane, the depth composition against a committed
#                      reference, its lighting against the same surfaces drawn as world geometry,
#                      the world's lens changed under it, its motion vectors carried and raised, the
#                      shadow policy, and its GPU time at 1080p. It compiles
#                      src/rendering/pipeline/tests/frame_scene.cpp and tests/render/golden.cpp.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory for the reason
# src/rendering/pipeline/tests/CMakeLists.txt gives.
set_property(GLOBAL PROPERTY CY_VIEWMODEL_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_viewmodel_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_VIEWMODEL_TESTS_DIR)
    foreach(kind IN ITEMS unit integration render)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/viewmodel/tests: the '${kind}' entry of the test taxonomy could "
                "not be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME render_viewmodel
        KIND unit
        SOURCES "${tests_dir}/test_viewmodel.cpp"
        DEPENDENCIES cy::rendering-viewmodel)
    cy_add_test(
        NAME viewmodel_assembly
        KIND integration
        SOURCES "${tests_dir}/test_viewmodel_assembly.cpp"
        DEPENDENCIES cy::rendering-viewmodel)

    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    set(pipeline_tests "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests")
    cy_add_test(
        NAME weapon_view
        KIND render
        SOURCES
            "${tests_dir}/test_weapon_view_device.cpp"
            "${pipeline_tests}/frame_scene.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-viewmodel cy::rendering-pipeline cy::rendering-particles
                     cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_weapon_view
                               PRIVATE "${tests_dir}" "${pipeline_tests}"
                                       "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_test_render_weapon_view
                               PRIVATE CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
                                       CY_VIEWMODEL_TEST_DIR="${tests_dir}"
                                       CY_PIPELINE_TEST_DIR="${pipeline_tests}")
    set_tests_properties(render.weapon_view PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_viewmodel_declare_tests)
