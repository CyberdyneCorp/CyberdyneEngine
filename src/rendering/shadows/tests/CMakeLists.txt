# src/rendering/shadows/tests/ — the virtual shadow suite. Section 8 of the M7 tasks.
#
# Declared through the same deferral src/rendering/culling/tests/ uses: cy_add_test() is defined by
# tests/CMakeLists.txt, which the top-level CMakeLists.txt adds *after* src/, so the declaration
# waits until the end of the top-level directory's processing and the budget is read back out of
# tests/ rather than restated.
#
#   render_shadows          every case at unit cost. No device and no fixture: a page cache, a
#                           clipmap, an address space and a fallback chain are deterministic
#                           arithmetic, and each case runs in microseconds.
#   render_shadow_control   integration, for two reasons. It is a SWEEP: 41 load magnitudes, each held for
#                           700 frames and released. `design.md` §2.1 measured what certifying a
#                           ladder controller on a single load is worth — the spike's own first law
#                           passed one load and oscillated on 27 of 71 — so the certification is the
#                           sweep and it does not fit a unit budget. And it holds the camera-cut
#                           case, which is two thousand dirty pages against an allocation that
#                           affords a hundred — shrinking that to fit a unit budget would certify a
#                           different scenario from the one the requirement is about.
set_property(GLOBAL PROPERTY CY_RENDER_SHADOWS_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_render_shadows_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_RENDER_SHADOWS_TESTS_DIR)

    foreach(kind unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/shadows/tests: the '${kind}' entry of the test taxonomy could not "
                "be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME render_shadows
        KIND unit
        SOURCES
            "${tests_dir}/test_address_space.cpp"
            "${tests_dir}/test_cache.cpp"
            "${tests_dir}/test_clipmap.cpp"
            "${tests_dir}/test_pages.cpp"
        DEPENDENCIES cy::rendering-shadows)

    cy_add_test(
        NAME render_shadow_control
        KIND integration
        SOURCES
            "${tests_dir}/test_budget_sweep.cpp"
            "${tests_dir}/test_camera_cut.cpp"
            "${tests_dir}/test_teardown.cpp"
        DEPENDENCIES cy::rendering-shadows)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_render_shadows_declare_tests)
