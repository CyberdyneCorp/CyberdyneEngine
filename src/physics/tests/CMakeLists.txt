# src/physics/tests/ — the physics ECS bridge's suites. M8.a section 4.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory's processing and
# reading the taxonomy back out of tests/, exactly as src/rendering/scene/tests/ does — a budget
# copied into a second file is a budget that drifts.
#
#   physics_bridge     integration. Registration, body creation, the refusal that does not fail the
#                      world, a sphere landing on a box, the removal sweep, the stage the step runs
#                      in, and the eight components declared to the state hash. Every case builds a
#                      real ECS world, a real scene tree and the real reference backend; nothing is
#                      mocked, because the point of the bridge is the join between three modules and
#                      a mock of any of them would test the mock.
#
#                      IT WAS DECLARED `unit` AND M8.a's CLOSING GATE MOVED IT, on the same argument
#                      src/gameplay/play/tests/CMakeLists.txt already makes two directories away:
#                      "a session builds an ECS world, a scene tree and a physics world per case; a
#                      millisecond budget would be met by trimming it until it stopped simulating
#                      anything." This suite does exactly that and was declared the other way. It
#                      passed in Development at -O2 and failed in **Debug**, where `the bridge steps
#                      in the Physics stage and in no other` spends 1.119 ms of CPU against the unit
#                      suite's 1.000 ms and four of its neighbours sit between 0.51 and 0.74 —
#                      which is not a case over the line, it is a suite in the wrong tier. It was
#                      the one M8.a-owned entry in `m1:four-profiles`' Debug failure list.
#
#   physics_simulation integration. The artefact's own physics claim — a sphere authored above a box
#                      comes to REST on it — against Jolt, because the reference backend integrates
#                      motion and declares that it does not resolve contacts.
#
#   physics_teardown   integration, and task 4.4. Sixty-four play sessions created and destroyed
#                      with work in flight, four spinner threads holding the cores, and — where this
#                      build has Jolt — the same loop against the backend whose job bridge M5.5's
#                      gate found freeing its pool underneath a worker. A unit budget of one
#                      millisecond would be met by trimming the rounds until the race stopped
#                      reproducing, which is the opposite of the point.
#
# THE JOLT CASE IS BEHIND CY_PHYSICS AND THE SUITE IS NOT. `-D CY_PHYSICS=OFF` removes one test case
# and leaves the other five and the whole of physics_bridge running against the reference backend —
# which is the arrangement src/servers/physics/README.md argues for, and the opposite of M3's
# CY_RENDERER_VULKAN mistake.
set_property(GLOBAL PROPERTY CY_PHYSICS_BRIDGE_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_physics_bridge_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_PHYSICS_BRIDGE_TESTS_DIR)

    foreach(kind unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/physics/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
                "the deferral around it should go away — declare the suites from tests/unit/ "
                "instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME physics_bridge
        KIND integration
        SOURCES
            "${tests_dir}/test_bridge.cpp"
            "${tests_dir}/test_state_schema.cpp"
        DEPENDENCIES cy::physics-bridge cy::physics-reference)

    # `CY_PHYSICS` IS A CMAKE OPTION AND NOT A COMPILE DEFINITION, which cost one confused test run
    # to discover: `-D CY_PHYSICS=ON` links the Jolt backend and defines nothing, so a `#if
    # defined(CY_PHYSICS)` in a test source is false in a build that HAS Jolt. samples/04-character
    # has the same `#if` and gets away with it because its own CMakeLists.txt defines the macro on
    # the target beside linking the library. The same two lines are here, together, so the link and
    # the macro cannot come apart.
    set(backends cy::physics-bridge cy::physics-reference cy::core-jobs)
    set(backend_definitions "")
    if(CY_PHYSICS)
        list(APPEND backends cy::physics-jolt)
        list(APPEND backend_definitions CY_PHYSICS=1)
    endif()
    cy_add_test(
        NAME physics_teardown
        KIND integration
        SOURCES "${tests_dir}/test_teardown.cpp"
        DEPENDENCIES ${backends}
        DEFINITIONS ${backend_definitions})

    cy_add_test(
        NAME physics_simulation
        KIND integration
        SOURCES "${tests_dir}/test_simulation.cpp"
        DEPENDENCIES ${backends}
        DEFINITIONS ${backend_definitions})
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_physics_bridge_declare_tests)
