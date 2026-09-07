# src/servers/render/culling/tests/ — GPU-driven culling's suite. M6 task 8.5.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing and the taxonomy is read back out
# of tests/ rather than restated, exactly as the render server's own suite does.
#
# UNIT, and that is the point of the module's shape. Every case here runs the whole GPU culling
# algorithm — the three tests in their required order, LOD selection with hysteresis and cross-fade,
# visibility ranges, the HLOD hierarchy, shadow-caster sweep rejection, the depth pyramid and the
# two-pass scheme — with no device, no window and no job system. The fixtures publish tens of
# instances rather than thousands, because a GPU scene slot is 160 bytes and the properties being
# checked do not need more.
set_property(GLOBAL PROPERTY CY_SERVERS_RENDER_CULLING_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_servers_render_culling_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_SERVERS_RENDER_CULLING_TESTS_DIR)

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
            "src/servers/render/culling/tests: the 'unit' entry of the test taxonomy could not be "
            "read from tests/CMakeLists.txt.")
    endif()
    cy_add_test(
        NAME render_gpu_culling
        KIND unit
        SOURCES
            "${tests_dir}/test_gpu_cull.cpp"
            "${tests_dir}/test_hzb.cpp"
        DEPENDENCIES cy::servers-render-culling)

    # Teardown under load, at integration cost. See tests/test_teardown.cpp for why it is separate.
    cy_add_test(
        NAME render_gpu_culling_teardown
        KIND integration
        SOURCES "${tests_dir}/test_teardown.cpp"
        DEPENDENCIES cy::servers-render-culling)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_servers_render_culling_declare_tests)
