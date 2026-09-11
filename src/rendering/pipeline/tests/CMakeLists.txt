# src/rendering/pipeline/tests/ — the layer, with and without a device. M8.c tasks 1b.1 and 1b.2.
#
# TWO SUITES, AND THE SPLIT IS THE TAXONOMY'S.
#
#   render_pipeline   KIND integration. Every case builds a spatial index, a material table, a
#                     render graph and a null device, and the taxonomy names all four as what does
#                     NOT belong in `unit`. What it asks is what was DECIDED: how many stages carry
#                     a callback, how many ran, how many draws each recorded, and how many bytes the
#                     Prepare transfer moved. All of that is answerable with no GPU.
#
#   pipeline          KIND render. The same scene on Vulkan with validation and synchronisation
#                     validation on, the output image copied back off the device, and the three
#                     pictures this milestone commits written out. Declared ONLY when
#                     CY_RENDERER_VULKAN is on, for the reason tests/render/ gives: a suite that
#                     always skips is a suite whose failure nobody would notice. With the backend
#                     compiled in and no GPU it SKIPS LOUDLY.
#
# THE DEVICE SUITE COMPILES tests/render/golden.cpp. That file is the PNG encoder and the image
# comparison the golden suite already uses, and this one writes its captures with the same encoder
# rather than a second one — a reviewer opens both sets in the same viewer, and there is one decoder
# in the tree that reads what the engine writes.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing, exactly as
# src/rendering/assembly/tests/CMakeLists.txt does it.
set_property(GLOBAL PROPERTY CY_PIPELINE_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_pipeline_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_PIPELINE_TESTS_DIR)

    foreach(kind IN ITEMS integration render)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/pipeline/tests: the '${kind}' entry of the test taxonomy could not "
                "be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME render_pipeline
        KIND integration
        SOURCES
            "${tests_dir}/frame_scene.cpp"
            "${tests_dir}/test_pipeline.cpp"
        DEPENDENCIES cy::rendering-pipeline cy::rendering-particles cy::rhi-null)
    target_include_directories(cy_test_integration_render_pipeline PRIVATE "${tests_dir}")

    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    cy_add_test(
        NAME pipeline
        KIND render
        SOURCES
            "${tests_dir}/frame_scene.cpp"
            "${tests_dir}/test_pipeline_device.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-pipeline cy::rendering-particles cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_pipeline
                               PRIVATE "${tests_dir}" "${CMAKE_SOURCE_DIR}/tests/render")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_pipeline_declare_tests)
