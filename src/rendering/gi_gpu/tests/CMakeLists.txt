# src/rendering/gi_gpu/tests/ — CyberGI on a device. Issue #35, stages 1 and 2.
#
#   gi_gpu   KIND render, Vulkan only. Every dispatch against the host implementation it transcribes:
#            the uploaded field sphere traced ray for ray against `DistanceField::sphere_trace`, a
#            moved object re-uploading only its bricks, the device card radiance against the host
#            surface cache over the same scene frame after frame, and the per-frame update held to
#            its budget. It also writes the lit cards of a test scene, host and device side by side,
#            with tests/render/golden.cpp's PNG encoder.
#
# It includes src/rendering/gi/tests/support.h — the room the host GI suites are written against —
# rather than building a second room.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory for the reason
# src/rendering/pipeline/tests/CMakeLists.txt gives.
set_property(GLOBAL PROPERTY CY_GI_GPU_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_gi_gpu_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_GI_GPU_TESTS_DIR)
    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_render)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_render)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/rendering/gi_gpu/tests: the 'render' entry of the test taxonomy could not be "
            "read from tests/CMakeLists.txt.")
    endif()
    set(CY_TEST_BUDGET_render "${budget}")
    set(CY_TEST_TIMEOUT_render "${timeout}")

    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    cy_add_test(
        NAME gi_gpu
        KIND render
        SOURCES
            "${tests_dir}/test_gi_gpu_device.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-gi-gpu cy::rendering-gi cy::rendering-graph cy::rhi
                     cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_gi_gpu
                               PRIVATE "${tests_dir}" "${CMAKE_SOURCE_DIR}/src/rendering/gi/tests"
                                       "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_test_render_gi_gpu
                               PRIVATE CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(render.gi_gpu PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_gi_gpu_declare_tests)
