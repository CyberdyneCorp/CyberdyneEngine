# src/gameplay/play/tests/ — spawning and the play session. M8.a section 5.
#
#   gameplay_spawn   unit. Requests, policies, reservation, and `spawn_many`'s CONTRACT over eight
#                    instances. No physics, no renderer, no display — `gameplay-framework`'s
#                    "Headless operation" is a requirement rather than a build configuration.
#
#   gameplay_play    integration. A whole play session over a real `.cyworld`: enter, tick, pause,
#                    stop, and the document's bytes compared before and after. `integration` rather
#                    than `unit` because a session builds an ECS world, a scene tree and a physics
#                    world per case and runs up to a hundred and eighty steps; a millisecond budget
#                    would be met by trimming it until it stopped simulating anything. It also holds
#                    batch spawning's COST at two hundred instances, for the same reason.
#
# THE BATCH CASE WAS SPLIT IN TWO AT M8.a's CLOSING GATE, and the reason belongs beside the suites
# rather than only in the source. `gameplay_spawn` held one case that spawned sixty-four instances of
# one name; `SceneTree::create_node` scans siblings to make a name unique, so the cost is O(N^2) and
# sixty-four of them spent 1.25 to 2.02 ms of CPU against the unit suite's 1.000 ms budget on seven
# standalone runs in eight. Inside `just test-unit` it failed three runs in six, which made
# `just test-all` — and therefore `m0:test`, a criterion of every ledger since M0 — a coin toss. The
# budget instrument was right; the case was in the wrong suite. The contract stayed in `unit` at
# eight instances (0.10 ms) and the cost moved to `integration` at two hundred, which is the remedy
# `tests/harness/src/budget.cpp` prints in the failure and the one M7's gate applied to three
# material cases.
#
# THE CY_PHYSICS CASE AND THE MACRO GO TOGETHER, as they do in src/physics/tests/CMakeLists.txt: the
# option links the backend and defines nothing, so the `#if` in the source needs the definition
# added here beside the link. With the option off, one case compiles out and eight remain.
set_property(GLOBAL PROPERTY CY_GAMEPLAY_PLAY_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_gameplay_play_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_GAMEPLAY_PLAY_TESTS_DIR)

    foreach(kind unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/gameplay/play/tests: the '${kind}' entry of the test taxonomy could not be "
                "read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME gameplay_spawn
        KIND unit
        SOURCES "${tests_dir}/test_spawn.cpp"
        DEPENDENCIES cy::gameplay-play)

    set(backends cy::gameplay-play cy::physics-reference)
    set(backend_definitions "")
    if(CY_PHYSICS)
        list(APPEND backends cy::physics-jolt)
        list(APPEND backend_definitions CY_PHYSICS=1)
    endif()
    cy_add_test(
        NAME gameplay_play
        KIND integration
        SOURCES "${tests_dir}/test_play.cpp"
        DEPENDENCIES ${backends}
        DEFINITIONS ${backend_definitions})
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_gameplay_play_declare_tests)
