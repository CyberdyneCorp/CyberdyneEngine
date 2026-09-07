# src/rendering/virtual_geometry/tests/ — CyberGeometry's suites. M7 section 7.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing and the taxonomy is read back out
# of tests/ rather than restated. src/servers/render/virtual_texturing/ does the same and says why.
#
#   virtual_geometry             unit. The vocabulary alone: the screen-error projection, the
#                                selection test and the normal cone, on hand-built clusters. It
#                                cooks nothing, so a failure names the arithmetic.
#
#   virtual_geometry_build       integration rather than unit, and the reason is the budget: a unit
#                                case has 1 ms of CPU time and cooking a 1,280-triangle sphere into
#                                a multi-level hierarchy is tens of milliseconds. Everything that
#                                cooks is here — the hierarchy, the crack-free sweep, the asset's
#                                round trip and the reference traversal — so that the unit suite's
#                                budget stays meaningful instead of being raised to accommodate a
#                                cook.
#
#   virtual_geometry_residency   integration. The geometry cache under a stream of requests, and the
#                                teardown case: a cache destroyed with requests in flight, in a
#                                loop, which is the shape M5.5's gate found in Jolt's job bridge.
#
#   render.virtual_geometry_gpu  the compute path on a real device, compared against the reference
#                                buffer for buffer. Declared only when CY_RENDERER_VULKAN is on, and
#                                it skips loudly on a machine that has the backend and no GPU.
set_property(GLOBAL PROPERTY CY_VG_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_virtual_geometry_declare_tests)
    get_property(vg_tests_dir GLOBAL PROPERTY CY_VG_TESTS_DIR)

    foreach(kind IN ITEMS unit integration render)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/virtual_geometry/tests: the '${kind}' entry of the test taxonomy "
                "could not be read from tests/CMakeLists.txt. If the budget variables were "
                "renamed, this lookup and the deferral around it should go away — declare the "
                "suites from tests/${kind}/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME virtual_geometry
        KIND unit
        SOURCES "${vg_tests_dir}/test_cluster.cpp"
        DEPENDENCIES cy::rendering-virtual-geometry)

    cy_add_test(
        NAME virtual_geometry_residency
        KIND integration
        SOURCES "${vg_tests_dir}/test_residency.cpp"
        DEPENDENCIES cy::rendering-virtual-geometry)

    cy_add_test(
        NAME virtual_geometry_build
        KIND integration
        SOURCES
            "${vg_tests_dir}/test_asset.cpp"
            "${vg_tests_dir}/test_build.cpp"
            "${vg_tests_dir}/test_traversal.cpp"
        DEPENDENCIES cy::rendering-virtual-geometry)
    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()

    # The compute path on a real device, compared against the reference buffer for buffer. Declared
    # only when the Vulkan backend is compiled in — a suite that always skips is a suite whose
    # failure nobody would notice — and it skips loudly on a machine that has the backend and no GPU.
    cy_add_test(
        NAME virtual_geometry_gpu
        KIND render
        SOURCES
            "${vg_tests_dir}/test_gpu_traversal.cpp"
            "${vg_tests_dir}/test_visbuffer.cpp"
        DEPENDENCIES cy::rendering-virtual-geometry cy::rendering-graph cy::rhi-null cy::rhi-vulkan)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_virtual_geometry_declare_tests)
