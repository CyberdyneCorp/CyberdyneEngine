# src/rendering/gi_gpu/tests/ — CyberGI on a device. Issue #35, stages 1 to 5.
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
    # The quality suite with a device is a smoke suite: its budget is the taxonomy's too.
    get_directory_property(CY_TEST_BUDGET_smoke DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_smoke)
    get_directory_property(CY_TEST_TIMEOUT_smoke DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_smoke)

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

    # Stage 3: the radiance cache's gather and query and the screen, software and hardware tiers,
    # each against the host function it transcribes; the device's acceleration structures against
    # the service's decisions; the fallback, including a device created without ray queries.
    cy_add_test(
        NAME gi_gpu_tracing
        KIND render
        SOURCES
            "${tests_dir}/test_gi_gpu_tracing.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-gi-gpu cy::rendering-gi cy::rendering-graph
                     cy::rendering-raytracing cy::rendering-raytracing-gpu cy::rhi cy::rhi-null
                     cy::rhi-vulkan cy::shader)
    # The module's own src/ for gi_gpu_spirv.h: the layout case reflects the committed SPIR-V.
    target_include_directories(cy_test_render_gi_gpu_tracing
                               PRIVATE "${tests_dir}" "${CMAKE_SOURCE_DIR}/src/rendering/gi/tests"
                                       "${CMAKE_SOURCE_DIR}/tests/render"
                                       "${tests_dir}/../src")
    target_compile_definitions(cy_test_render_gi_gpu_tracing
                               PRIVATE CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(render.gi_gpu_tracing PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")

    # Stage 4: the per-pixel resolve and its denoiser against `gi::ScreenResolve` and
    # `denoise::Denoiser`, through both tiers, and what every stage costs at 1080p. The image's
    # quality is integration.render_gi_screen's, measured on the oracle this holds the device to.
    cy_add_test(
        NAME gi_gpu_resolve
        KIND render
        SOURCES "${tests_dir}/test_gi_gpu_resolve.cpp"
        DEPENDENCIES cy::rendering-gi-gpu cy::rendering-gi cy::rendering-graph
                     cy::rendering-arbiter cy::rendering-raytracing cy::rendering-raytracing-gpu
                     cy::rhi cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_gi_gpu_resolve
                               PRIVATE "${tests_dir}" "${CMAKE_SOURCE_DIR}/src/rendering/gi/tests"
                                       "${CMAKE_SOURCE_DIR}/tests/render")
    set_tests_properties(render.gi_gpu_resolve PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")

    # The screen-probe gather and the per-pixel gather at 1920x1080 and 4K: the guide's quality
    # axes — the path tracer, a 10 cm wall, a still view, a moving light — read back from the
    # published frame, and every stage's cost, through both tiers. A smoke suite with a device:
    # each case converges a full-resolution scene per gather.
    cy_add_test(
        NAME gi_gpu_probe_quality
        KIND smoke
        SOURCES "${tests_dir}/test_gi_gpu_probe_quality.cpp"
        DEPENDENCIES cy::rendering-gi-gpu cy::rendering-gi cy::rendering-graph
                     cy::rendering-raytracing cy::rendering-raytracing-gpu cy::rhi cy::rhi-null
                     cy::rhi-vulkan)
    target_include_directories(cy_test_smoke_gi_gpu_probe_quality
                               PRIVATE "${tests_dir}" "${CMAKE_SOURCE_DIR}/src/rendering/gi/tests"
                                       "${CMAKE_SOURCE_DIR}/tests/render")
    set_tests_properties(smoke.gi_gpu_probe_quality PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")

    # Stage 4 in the frame: the pipeline suites' scene with the `ScreenSpaceGi` stage produced by
    # `GpuScreenResolve` and composited by cy/frame.slang — inert until bound, the double-counting
    # rule at the composite, and the prepass unpacked into the resolve's surfaces. It compiles
    # src/rendering/pipeline/tests/frame_scene.cpp, the one scene, rather than a second.
    set(pipeline_tests "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests")
    cy_add_test(
        NAME gi_gpu_frame
        KIND render
        SOURCES
            "${tests_dir}/test_gi_gpu_frame.cpp"
            "${pipeline_tests}/frame_scene.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-gi-gpu cy::rendering-gi cy::rendering-pipeline
                     cy::rendering-particles cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_gi_gpu_frame
                               PRIVATE "${tests_dir}" "${CMAKE_SOURCE_DIR}/src/rendering/gi/tests"
                                       "${pipeline_tests}" "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_test_render_gi_gpu_frame
                               PRIVATE CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(render.gi_gpu_frame PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_gi_gpu_declare_tests)
