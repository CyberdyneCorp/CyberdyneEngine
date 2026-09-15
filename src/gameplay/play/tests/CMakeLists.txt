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

    # --- M11.b: the three play modes, and a live edit that reaches the world they carry ----------
    #
    # `editor_play`      integration. The three modes driven by ONE command stream, the refusal a
    #                    mode that is not available produces, and stepping. Its name is the editor's
    #                    rather than the module's because the requirement is the editor's: `Play
    #                    modes` is one requirement in `editor-architecture` and `live-editing`, and
    #                    `m11b:play-mode-round-trip` names this binary.
    #
    # `editor_live_edit` integration. A field edited while a world is playing, under each of the six
    #                    policies. Links `cy::gameplay-live` — a test consumes the engine rather than
    #                    sitting in it, so a suite under play/ may link the module above it.
    #
    # BOTH ARE DECLARED HERE RATHER THAN IN tests/integration/, and the reason is mechanical as well
    # as conceptual: a suite declared in `tests/integration/` lands at `tests/integration/` in the
    # build tree, and one declared through this deferred function lands at the build root, which is
    # where the milestone ledger's criteria look for a binary. The conceptual half is that both are
    # about a play session — the modes are its transport and the live edit is a change reaching it —
    # so the module that owns the session owns their suites.
    #
    # IT LINKS A PLATFORM AND IT DEPENDS ON A BINARY, and both are new at the repair round that made
    # `separate-process` a process. `cy::platform-desktop-sdl3` is the only implementation of
    # `Platform` in the tree and therefore the only thing that can start one; `cy_play_runtime_host`
    # is what it starts, and the explicit dependency is what stops the suite from being run against
    # a tree where the host binary has not been built yet — which would present as the mode being
    # unavailable rather than as the missing build step it is.
    cy_add_test(
        NAME editor_play
        KIND integration
        SOURCES "${tests_dir}/test_editor_play.cpp"
        DEPENDENCIES cy::core-assets cy::gameplay-play cy::physics-reference
                     cy::platform-desktop-sdl3 cy::scene cy::scene-serialization)
    add_dependencies(cy_test_integration_editor_play cy_play_runtime_host)

    cy_add_test(
        NAME editor_live_edit
        KIND integration
        SOURCES "${tests_dir}/test_editor_live_edit.cpp"
        DEPENDENCIES cy::gameplay-live cy::gameplay-play cy::physics-reference cy::scene
                     cy::scene-serialization)

    # --- The contract the two suites above assume, checked where it cannot stop firing ------------
    #
    # A suite proves the engine does what the suite asks. It cannot prove that what it asks is what
    # the specification asks, or that the editor on the other side of the boundary asks the same
    # thing — and that gap is exactly where `editor-architecture` and `live-editing` spent five
    # milestones at Seed. `tools/editor/play_contract.py` derives the play-mode and live-edit-policy
    # tables from the engine's own declarations, from the editor's, and from the requirements' own
    # markdown tables, and requires the three to agree.
    #
    # Registered here rather than left to a recipe, for the reason src/abi/tests/CMakeLists.txt
    # gives about the ABI gate: a check that runs only when somebody remembers the recipe is a check
    # that has silently stopped firing. Neither entry needs a built engine, so both cost
    # milliseconds and run in every profile.
    #
    #   integration.editor_contract_play_modes
    #   integration.editor_contract_live_edit_policy
    #                                 the tree satisfies each contract. These are the gate.
    #   integration.editor_contract_gate  the gate still REFUSES a renamed mode, a fourth mode on
    #                                     one side only, a capability the two sides disagree about,
    #                                     a refusal that names no rung, a policy the specification
    #                                     drops, two policies sharing a disturbance rank, and a
    #                                     classification from which a restart becomes derivable.
    find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)

    foreach(contract play-modes live-edit-policy)
        string(REPLACE "-" "_" contract_test "${contract}")
        add_test(NAME integration.editor_contract_${contract_test}
            COMMAND ${Python3_EXECUTABLE}
                    "${CMAKE_SOURCE_DIR}/tools/editor/play_contract.py" "${contract}")
        set_tests_properties(integration.editor_contract_${contract_test}
            PROPERTIES TIMEOUT 300 LABELS integration)
    endforeach()

    add_test(NAME integration.editor_contract_gate
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/editor/selftest.py")
    set_tests_properties(integration.editor_contract_gate PROPERTIES TIMEOUT 300 LABELS integration)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_gameplay_play_declare_tests)
