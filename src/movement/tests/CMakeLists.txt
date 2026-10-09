# src/movement/tests/ — the fixed-point mover and the Fixed navigation world. openspec/changes/
# add-deterministic-math, stage 6 and the lockstep half of stage 7.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines after src/ is read, so the
# declaration is deferred to the end of the top-level directory's processing and the taxonomy's
# budgets are read back out of tests/ rather than restated (src/navigation/tests/ does the same).
#
#   unit.movement          the converted mesh, the path queries, the flow field, the height field,
#                          the mover's passes
#                          one by one, the authoritative transform and its presentation sync, and
#                          the profile check with this module linked. Every case
#                          builds a mesh of a few dozen polygons and a handful of units.
#
#   integration.movement_crowd
#                          navigation's crowd solver instantiated over Fixed (task 6.2): a head-on
#                          pair passes, priority yields, it chooses the f32 crowd's side, its
#                          committed digest, and avoidance fed through the mover.
#
#   integration.movement_lockstep
#                          the lockstep RTS scenario (rts_scenario.h): two sessions in one process
#                          driven by one command log, compared tick by tick; the serial step
#                          against the step on job workers; and the scenario's committed digest.
#                          Hundreds of units for hundreds of ticks is over the unit tier by
#                          construction.
#
# nav_fixture.h is src/navigation/tests/'s: the converted mesh is converted FROM the meshes those
# suites build, so the two suites test one mesh rather than two that happen to look alike.
set_property(GLOBAL PROPERTY CY_MOVEMENT_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_movement_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_MOVEMENT_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/movement/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME movement
        KIND unit
        SOURCES
            "${tests_dir}/test_components.cpp"
            "${tests_dir}/test_flow_field.cpp"
            "${tests_dir}/test_height_field.cpp"
            "${tests_dir}/test_mover.cpp"
            "${tests_dir}/test_nav_mesh.cpp"
            "${tests_dir}/test_path.cpp"
            "${tests_dir}/test_profile.cpp"
        DEPENDENCIES cy::movement cy::gameplay cy::gameplay-abilities)
    target_include_directories(cy_test_unit_movement
                               PRIVATE "${CMAKE_SOURCE_DIR}/src/navigation/tests")

    cy_add_test(
        NAME movement_crowd
        KIND integration
        SOURCES "${tests_dir}/test_crowd.cpp"
        DEPENDENCIES cy::movement)

    cy_add_test(
        NAME movement_lockstep
        KIND integration
        SOURCES "${tests_dir}/test_lockstep.cpp"
        DEPENDENCIES cy::movement cy::gameplay cy::core-jobs)
    target_include_directories(cy_test_integration_movement_lockstep
                               PRIVATE "${CMAKE_SOURCE_DIR}/src/navigation/tests")

    # The profile seam (test_profile.cpp) reads `from_build()` in this suite, so the suite is
    # covered by a profile the way src/core/detmath/tests/ covers its own.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(cy_test_unit_movement PRIVATE -ffp-contract=off)
    elseif(MSVC)
        target_compile_options(cy_test_unit_movement PRIVATE /fp:precise)
    endif()
    cy_declare_determinism_profile(cy_test_unit_movement
        PROFILE Lockstep
        REASON "it asserts what the profile check answers for a lockstep session with the mover and the deterministic math module linked")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_movement_declare_tests)
