# src/servers/render/geometry/tests/ — M6 task 8.4's suite.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory's processing for the
# reason the render server's own tests/CMakeLists.txt gives, and reading the taxonomy back out of
# tests/ rather than restating it.
#
# UNIT. Deferred release under frames in flight, the mip tail's guarantee, the ring's refusal to wrap
# and the blend-shape compaction are all arithmetic over small fixtures, and none of them needs a
# device — which is the property the module's layer buys.
set_property(GLOBAL PROPERTY CY_RENDER_GEOMETRY_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_render_geometry_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_RENDER_GEOMETRY_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(kind_budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(kind_timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        set(CY_TEST_BUDGET_${kind} "${kind_budget}")
        set(CY_TEST_TIMEOUT_${kind} "${kind_timeout}")
    endforeach()
    set(budget "${CY_TEST_BUDGET_unit}")
    set(timeout "${CY_TEST_TIMEOUT_unit}")
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/servers/render/geometry/tests: the 'unit' entry of the test taxonomy could not be "
            "read from tests/CMakeLists.txt.")
    endif()
    cy_add_test(
        NAME render_geometry
        KIND unit
        SOURCES
            "${tests_dir}/test_skinning.cpp"
            "${tests_dir}/test_skin_dispatch.cpp"
            "${tests_dir}/test_resources.cpp"
        DEPENDENCIES cy::servers-render-geometry)

    # Teardown under load, at integration cost. See tests/test_teardown.cpp for why it is separate.
    cy_add_test(
        NAME render_geometry_teardown
        KIND integration
        SOURCES "${tests_dir}/test_teardown.cpp"
        DEPENDENCIES cy::servers-render-geometry)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_render_geometry_declare_tests)
