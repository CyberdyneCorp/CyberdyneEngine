# src/replay/tests/ — CyberReplay's suites. M9 section 1.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory's processing and
# reading the taxonomy back out of tests/, exactly as src/gameplay/tests/ does — a budget copied
# into a second file is a budget that drifts.
#
#   replay_core      arithmetic over records, ledgers and cursors. No world, no device.
#   replay_log       `integration`: the write-and-read-back case is six hundred records through the
#                    encoder, the compressor and back, and it measured 1.035 ms in Development.
#   replay_snapshot  `integration`: snapshot kinds, the checkpoint policy, the bounded window — and
#                    test_teardown.cpp, which drives every bounded structure PAST its bound and then
#                    asks a TrackingAllocator what is left. Eviction is where a bounded structure
#                    leaks, because the object that goes away is the one nobody is looking at.
#   replay_snapshot  `integration`, because every case builds an `ecs::World`, registers components
#                    and captures it. The taxonomy's rule is about cost, not subject: a case that
#                    builds a world does not belong in a tier budgeted at one millisecond.
#
# THE FILE TO READ FIRST IS test_one_record.cpp. It is task 1.5 and the whole of the milestone's
# subtitle: five readers, one record type, checked rather than reviewed. Its header comment lists
# the three one-line mutations that turn it red, and src/replay/README.md pastes the run.
set_property(GLOBAL PROPERTY CY_REPLAY_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_replay_declare_tests)
    get_property(cy_replay_tests_dir GLOBAL PROPERTY CY_REPLAY_TESTS_DIR)

    foreach(kind unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/replay/tests: the '${kind}' entry of the test taxonomy could not be read from "
                "tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
                "deferral around it should go away — declare the suites from tests/unit/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME replay_core
        KIND unit
        SOURCES
            "${cy_replay_tests_dir}/test_divergence.cpp"
            "${cy_replay_tests_dir}/test_external.cpp"
            "${cy_replay_tests_dir}/test_ledger.cpp"
            "${cy_replay_tests_dir}/test_lockstep.cpp"
            "${cy_replay_tests_dir}/test_one_record.cpp"
            "${cy_replay_tests_dir}/test_playback_clock.cpp"
            "${cy_replay_tests_dir}/test_readers.cpp"
            "${cy_replay_tests_dir}/test_record.cpp"
        DEPENDENCIES cy::replay)

    # replay_log is `integration` and not `unit` because of a measurement, not a preference: its
    # write-and-read-back case encodes, compresses, expands and decodes six hundred records and
    # measured 1.035 ms of CPU in Development against the unit tier's 1 ms budget — before Debug,
    # where `-O0` makes the same path several times slower. The remedy is the one the taxonomy
    # states and M8.a's gate applied to batch spawning: move the case, do not trim the population
    # until the property stops being about a log.
    cy_add_test(
        NAME replay_log
        KIND integration
        SOURCES "${cy_replay_tests_dir}/test_log.cpp"
        DEPENDENCIES cy::replay)

    cy_add_test(
        NAME replay_snapshot
        KIND integration
        SOURCES
            "${cy_replay_tests_dir}/test_snapshot.cpp"
            "${cy_replay_tests_dir}/test_teardown.cpp"
            "${cy_replay_tests_dir}/test_teardown_playback.cpp"
        DEPENDENCIES cy::replay)

    # M9 section 3. `integration` and not `unit` for the taxonomy's stated reason and not a
    # preference: every case in these four files builds an `ecs::World`, four participants, a command
    # stream and two hashing codecs, then runs twenty-five to forty ticks of it twice. A case that
    # builds a world does not belong in a tier budgeted at one millisecond of CPU, and the remedy the
    # taxonomy names is to move the case rather than to shrink it until the property stops being
    # about a session.
    cy_add_test(
        NAME replay_session
        KIND integration
        SOURCES
            "${cy_replay_tests_dir}/test_bitexact.cpp"
            "${cy_replay_tests_dir}/test_crash.cpp"
            "${cy_replay_tests_dir}/test_playback.cpp"
            "${cy_replay_tests_dir}/test_rollback.cpp"
        DEPENDENCIES cy::replay)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_replay_declare_tests)
