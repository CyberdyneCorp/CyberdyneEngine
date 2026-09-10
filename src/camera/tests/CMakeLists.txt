# src/camera/tests/ — the suites for CyberCamera's world half. M8.b task 7.3.
#
# TWO KINDS. `unit` holds the arithmetic — framing, constraints, the volume index, the projection
# utilities, the zoom curve, edge scrolling, aim assistance and the director — none of which builds a
# world or a device. `integration` holds the case that authors a CyberGraph rig graph, turns it into
# a definition and drives `CameraServer` over the compiled program: a graph compiler running inside a
# suite with a one-millisecond budget is a suite that goes red on a loaded machine.

# The property and the function carry `WORLD` in their names because src/servers/camera/tests/ uses
# `CY_CAMERA_TESTS_DIR` and `cy_camera_declare_tests`, and CMake functions and global properties are
# both global: reusing either name would silently redefine that module's declaration and its suite
# would be declared from this directory. It cost one configure to find, and the name is the fix.
set_property(GLOBAL PROPERTY CY_CAMERA_WORLD_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_camera_world_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_CAMERA_WORLD_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/camera/tests: the '${kind}' entry of the test taxonomy could not be read from "
                "tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME camera_world
        KIND unit
        SOURCES
            "${tests_dir}/test_framing.cpp"
            "${tests_dir}/test_projection.cpp"
            "${tests_dir}/test_strategy.cpp"
        DEPENDENCIES cy::camera)

    cy_add_test(
        NAME camera_authoring
        KIND integration
        SOURCES
            "${tests_dir}/test_authoring.cpp"
        DEPENDENCIES cy::camera)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_camera_world_declare_tests)
