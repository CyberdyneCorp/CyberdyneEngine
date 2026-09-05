# src/servers/physics/tests/ — the physics server's suites. Section 4.2 of the M4 tasks.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing, and the taxonomy is read back out
# of tests/ rather than restated, because a budget copied into a second file is a budget that
# drifts. src/servers/render/, src/scene/ and src/ecs/ all do the same, for the same reason.
#
#   physics_server       unit. The vocabulary, shapes, bodies, events, queries and the 2D API. No
#                        device, no window, no ECS world and no job system in any of them: physics
#                        is layer 2 and cannot reach one, which is what makes "no gameplay code
#                        changes when the backend changes" a property of the build.
#
#   physics_behaviour    integration. The character controller, the fixed-step integration and the
#                        determinism probe. These are the cases that run a scene for a hundred-odd
#                        ticks, and a unit budget of one millisecond of CPU is measured per CASE —
#                        a 120-tick determinism run over six bodies, twice, does not fit and should
#                        not be made to.
#
# THE SPLIT IS BY COST, NOT BY IMPORTANCE. `just test-unit` stays under a minute, and the cases that
# genuinely need a second are declared as what they are rather than trimmed until they fit.
set_property(GLOBAL PROPERTY CY_PHYSICS_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_physics_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_PHYSICS_TESTS_DIR)

    foreach(kind unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/servers/physics/tests: the '${kind}' entry of the test taxonomy could not be "
                "read from tests/CMakeLists.txt. If the budget variables were renamed, this lookup "
                "and the deferral around it should go away — declare the suites from tests/ "
                "instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME physics_server
        KIND unit
        SOURCES
            "${tests_dir}/test_events.cpp"
            "${tests_dir}/test_physics2d.cpp"
            "${tests_dir}/test_queries.cpp"
            "${tests_dir}/test_server.cpp"
            "${tests_dir}/test_shapes.cpp"
            "${tests_dir}/test_types.cpp"
        DEPENDENCIES cy::servers-physics cy::physics-reference)

    cy_add_test(
        NAME physics_behaviour
        KIND integration
        SOURCES
            "${tests_dir}/test_character.cpp"
            "${tests_dir}/test_determinism.cpp"
            "${tests_dir}/test_stepper.cpp"
        DEPENDENCIES cy::servers-physics cy::physics-reference)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_physics_declare_tests)
