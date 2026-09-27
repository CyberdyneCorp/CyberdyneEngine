# src/rendering/contact_shadows/tests/ — contact shadows on the host, and soft and contact shadows
# on a device.
#
#   rendering_contact_shadows   KIND integration. `contact_shadow_reference` over contact_scene.h's
#                               analytic floor and box: an open plane traces to exactly one at every
#                               light elevation, a contact darkens where the geometry puts the box
#                               between the floor and the light and nowhere else, and the budget's
#                               refinement lever removes the trace. No device. Integration rather
#                               than unit: each case traces a 384x256 image, tens of milliseconds.
#
#   soft_shadows                KIND render, Vulkan only. The trace against the host reference on
#                               the device, and the frame the pipeline suites render given a
#                               directional shadow map: off identical to a committed reference drawn
#                               by the frame shader before this change, the penumbra widening with
#                               the blocker's distance, no leak into an umbra and no speckle on an
#                               open floor, and the contact term darkening where a coarse map misses
#                               the contact and nowhere else. It compiles
#                               src/rendering/pipeline/tests/frame_scene.cpp — the one scene — and
#                               tests/render/golden.cpp for the PNG encoder.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory for the reason
# src/rendering/pipeline/tests/CMakeLists.txt gives.
set_property(GLOBAL PROPERTY CY_CONTACT_SHADOW_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_contact_shadows_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_CONTACT_SHADOW_TESTS_DIR)
    foreach(kind IN ITEMS integration render)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/contact_shadows/tests: the '${kind}' entry of the test taxonomy "
                "could not be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME rendering_contact_shadows
        KIND integration
        SOURCES "${tests_dir}/test_contact.cpp"
        DEPENDENCIES cy::rendering-contact-shadows)
    target_include_directories(cy_test_integration_rendering_contact_shadows
                               PRIVATE "${tests_dir}")

    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    set(pipeline_tests "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests")
    cy_add_test(
        NAME soft_shadows
        KIND render
        SOURCES
            "${tests_dir}/test_soft_shadows_device.cpp"
            "${pipeline_tests}/frame_scene.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-contact-shadows cy::rendering-lighting cy::rendering-pipeline
                     cy::rendering-particles cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_soft_shadows
                               PRIVATE "${tests_dir}" "${pipeline_tests}"
                                       "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_test_render_soft_shadows
                               PRIVATE CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
                                       CY_CONTACT_TEST_DIR="${tests_dir}")
    set_tests_properties(render.soft_shadows PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_contact_shadows_declare_tests)
