# src/servers/render/tests/ — the render server's suites. Section 4.1 and task 4.2.1.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing, and the taxonomy is read back out
# of tests/ rather than restated, because a budget copied into a second file is a budget that
# drifts. src/scene/, src/ecs/ and src/rendering/graph/ all do the same, for the same reason.
#
#   render_server  every case at unit cost. There is no device, no window, no ECS world and no job
#                  system in any of them — the render server is layer 2 and cannot reach one, which
#                  is what makes `rendering-architecture`'s "a test drives RenderServer directly
#                  with handles and produces a frame without an ECS world or scene tree existing" a
#                  property of the build rather than of a mock.
#
#                  The one thing to watch when adding a case: a GPU scene slot is 160 bytes and
#                  `initialize()` clears every one, so a capacity in the thousands is a memset a
#                  millisecond budget at -O0 would notice. The fixtures here use 8 to 64 slots,
#                  which is what the properties being checked need.
set_property(GLOBAL PROPERTY CY_RENDER_SERVER_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_render_server_declare_tests)
    get_property(cy_server_tests_dir GLOBAL PROPERTY CY_RENDER_SERVER_TESTS_DIR)

    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_unit)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_unit)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/servers/render/tests: the 'unit' entry of the test taxonomy could not be read "
            "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
            "deferral around it should go away — declare the suites from tests/unit/ instead.")
    endif()
    set(CY_TEST_BUDGET_unit "${budget}")
    set(CY_TEST_TIMEOUT_unit "${timeout}")

    cy_add_test(
        NAME render_server
        KIND unit
        SOURCES
            "${cy_server_tests_dir}/test_debug_draw.cpp"
            "${cy_server_tests_dir}/test_gpu_scene.cpp"
            "${cy_server_tests_dir}/test_mesh.cpp"
            "${cy_server_tests_dir}/test_server.cpp"
            "${cy_server_tests_dir}/test_snapshot.cpp"
            "${cy_server_tests_dir}/test_sort.cpp"
            "${cy_server_tests_dir}/test_texture.cpp"
            "${cy_server_tests_dir}/test_view.cpp"
        DEPENDENCIES cy::servers-render)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_render_server_declare_tests)
