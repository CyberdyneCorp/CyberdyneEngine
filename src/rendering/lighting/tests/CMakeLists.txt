# src/rendering/lighting/tests/ — the lighting suite. Section 4.4 of the M3 tasks.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing, and the taxonomy is read back out
# of tests/ rather than restated, because a budget copied into a second file is a budget that drifts.
#
#   render_lighting  every case at unit cost, and none of them needs a device. The cascade cases are
#                    the ones worth knowing about: "shadows do not swim as the camera rotates" is a
#                    property of two matrices, so it is asserted numerically rather than photographed
#                    much later and much less precisely.
set_property(GLOBAL PROPERTY CY_RENDER_LIGHTING_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_render_lighting_declare_tests)
    get_property(cy_lighting_tests_dir GLOBAL PROPERTY CY_RENDER_LIGHTING_TESTS_DIR)

    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_unit)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_unit)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/rendering/lighting/tests: the 'unit' entry of the test taxonomy could not be read "
            "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
            "deferral around it should go away — declare the suites from tests/unit/ instead.")
    endif()
    set(CY_TEST_BUDGET_unit "${budget}")
    set(CY_TEST_TIMEOUT_unit "${timeout}")

    cy_add_test(
        NAME render_lighting
        KIND unit
        SOURCES
            "${cy_lighting_tests_dir}/test_shadows.cpp"
            "${cy_lighting_tests_dir}/test_units.cpp"
        DEPENDENCIES cy::rendering-lighting)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_render_lighting_declare_tests)
