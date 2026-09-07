# src/servers/render/virtual_texturing/tests/ — CyberTexture's suites. M6 section 5.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing and the taxonomy is read back out
# of tests/ rather than restated. src/servers/render/, src/servers/camera/ and src/servers/residency/
# all do the same.
#
#   virtual_texturing        unit. No thread, no device and no shader in any of them — this module
#                            is layer 2 and may name none of those. test_system.cpp carries half of
#                            M6's exit criterion for the capability: **the mip tail guarantees a
#                            frame is never missing**, asserted over the whole address space of a
#                            texture rather than at a few sampled points.
#
#   virtual_texturing_teardown
#                            integration, because it spawns threads and joins them. It destroys a
#                            hundred systems with production in flight, unregisters a texture whose
#                            pages are being written, and puts eight recording threads against a
#                            churning resolver — which is the other half of the exit criterion,
#                            **feedback never blocks a frame**. The unit suite checks the shape that
#                            makes that true; this one checks it under contention.
set_property(GLOBAL PROPERTY CY_VT_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_virtual_texturing_declare_tests)
    get_property(vt_tests_dir GLOBAL PROPERTY CY_VT_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/servers/render/virtual_texturing/tests: the '${kind}' entry of the test "
                "taxonomy could not be read from tests/CMakeLists.txt. If the budget variables "
                "were renamed, this lookup and the deferral around it should go away — declare "
                "the suites from tests/${kind}/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME virtual_texturing
        KIND unit
        SOURCES
            "${vt_tests_dir}/test_address.cpp"
            "${vt_tests_dir}/test_feedback.cpp"
            "${vt_tests_dir}/test_page_table.cpp"
            "${vt_tests_dir}/test_physical_cache.cpp"
            "${vt_tests_dir}/test_system.cpp"
        DEPENDENCIES cy::servers-render-virtual-texturing)

    cy_add_test(
        NAME virtual_texturing_teardown
        KIND integration
        SOURCES "${vt_tests_dir}/test_teardown.cpp"
        DEPENDENCIES cy::servers-render-virtual-texturing)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_virtual_texturing_declare_tests)
