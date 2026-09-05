# src/servers/camera/tests/ — the camera system's suites. Tasks 4.3.1 to 4.3.3.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing and the taxonomy is read back out of
# tests/ rather than restated, because a budget copied into a second file is a budget that drifts.
# src/servers/render/, src/scene/, src/ecs/ and src/rendering/graph/ all do the same.
#
#   camera_system  every case at unit cost. There is no world, no entity, no device and no renderer
#                  in any of them, because layer 2 forbids all four — which is what makes
#                  `camera-system`'s "a debug camera, a reflection capture, and an editor viewport
#                  SHALL each be a camera rig with no requirement for a gameplay entity" a property
#                  of the build rather than of a mock.
set_property(GLOBAL PROPERTY CY_CAMERA_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_camera_declare_tests)
    get_property(camera_tests_dir GLOBAL PROPERTY CY_CAMERA_TESTS_DIR)

    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_unit)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_unit)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/servers/camera/tests: the 'unit' entry of the test taxonomy could not be read "
            "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
            "deferral around it should go away — declare the suites from tests/unit/ instead.")
    endif()
    set(CY_TEST_BUDGET_unit "${budget}")
    set(CY_TEST_TIMEOUT_unit "${timeout}")

    cy_add_test(
        NAME camera_system
        KIND unit
        SOURCES
            "${camera_tests_dir}/test_camera.cpp"
            "${camera_tests_dir}/test_lens.cpp"
            "${camera_tests_dir}/test_rig.cpp"
            "${camera_tests_dir}/test_server.cpp"
            "${camera_tests_dir}/test_smoothing.cpp"
            "${camera_tests_dir}/test_stack.cpp"
            "${camera_tests_dir}/test_view.cpp"
        DEPENDENCIES cy::servers-camera)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_camera_declare_tests)
