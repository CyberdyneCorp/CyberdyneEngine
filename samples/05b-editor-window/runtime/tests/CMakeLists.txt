# samples/05b-editor-window/runtime/tests/ — the runtime's decisions, without a device.
#
# The association, the transaction decoding and the overlay's rasteriser are the parts of this
# runtime that can be wrong quietly: an identity that moves between objects, a transaction half
# applied, a handle drawn in the wrong colour or in the wrong place. None of them needs Vulkan, so
# none of them waits for `smoke.editor_window` — which needs a display and is not in the permanent
# gate set.
set_property(GLOBAL PROPERTY CY_EDITOR_WINDOW_RUNTIME_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_editor_window_runtime_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_EDITOR_WINDOW_RUNTIME_TESTS_DIR)
    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "samples/05b-editor-window/runtime/tests: the '${kind}' entry of the test taxonomy "
                "could not be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME editor_window_runtime
        KIND unit
        SOURCES
            "${tests_dir}/test_runtime.cpp"
            "${tests_dir}/../overlay.cpp"
            "${tests_dir}/../session.cpp"
        DEPENDENCIES cy::servers-render)

    # The one case in this directory that does not fit a millisecond, and the file says why it
    # cannot be made to. M7's closing gate measured it at 0.81 to 0.98 ms of CPU in the Debug
    # configuration against a 1.00 ms unit budget, and at 1.94 ms on a machine that was building.
    cy_add_test(
        NAME editor_window_overlay
        KIND integration
        SOURCES
            "${tests_dir}/test_overlay_clipping.cpp"
            "${tests_dir}/../overlay.cpp"
        DEPENDENCIES cy::servers-render)

    foreach(target cy_test_unit_editor_window_runtime cy_test_integration_editor_window_overlay)
        target_include_directories(${target} PRIVATE "${tests_dir}/..")
    endforeach()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_editor_window_runtime_declare_tests)
