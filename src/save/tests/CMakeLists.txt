# src/save/tests/ — the save module's suites. Section 6 of the M6 tasks.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred to the end of the top-level directory's processing, by which time the
# taxonomy has been declared; the taxonomy itself is read back out of tests/ rather than restated,
# because a budget copied into a second file is a budget that drifts. src/core/serialize/tests/ and
# every other module with its own suites do the same, for the same reason.
#
# THREE SUITES, AND THE SPLIT IS THE TAXONOMY'S — ABOUT COST, NOT ABOUT SUBJECT.
#
#   save            the model: traits, the overlay, the container, migration and the diagnostics of
#                   a malformed save. A case here is a handful of records and a few hundred bytes.
#   save_fuzz       the two exhaustive sweeps: a chunk truncated at every length and corrupted at
#                   every byte. A few hundred decodes each, which is three times the unit budget in
#                   the debug profile — so it is here rather than shrunk, because the claim is
#                   "at every truncation" and "at every byte".
#   save_archive    generations, retention, the journal, compaction and the transactional case that
#                   fails a write at every step. Each case commits several generations and hashes
#                   every chunk in them, which is over the unit budget by design rather than by
#                   accident.
#   save_kill_nine  the exit criterion: a child process SIGKILLed at each phase of a real commit to
#                   a real directory. It forks, so it is an integration case by construction — and
#                   it is separate from save_archive so that a failure names the criterion.
#   save_service    the background writer, and teardown while a write is in flight. It starts a job
#                   system and an async service, forty times over in one case.

set_property(GLOBAL PROPERTY CY_SAVE_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_save_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_SAVE_TESTS_DIR)

    # cy_add_test() reads the taxonomy from variables that tests/CMakeLists.txt sets in its own
    # directory scope, and this function runs in the top-level scope, which is not a child of it.
    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/save/tests: the '${kind}' entry of the test taxonomy could not be read from "
                "tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
                "deferral around it should go away — declare the suites from tests/unit/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME save
        KIND unit
        SOURCES
            "${tests_dir}/test_container.cpp"
            "${tests_dir}/test_overlay.cpp"
            "${tests_dir}/test_traits.cpp"
        DEPENDENCIES cy::save)

    cy_add_test(
        NAME save_fuzz
        KIND integration
        SOURCES "${tests_dir}/test_fuzz.cpp"
        DEPENDENCIES cy::save)

    cy_add_test(
        NAME save_archive
        KIND integration
        SOURCES "${tests_dir}/test_archive.cpp"
        DEPENDENCIES cy::save)

    cy_add_test(
        NAME save_kill_nine
        KIND integration
        SOURCES "${tests_dir}/test_kill_nine.cpp"
        DEPENDENCIES cy::save)

    cy_add_test(
        NAME save_service
        KIND integration
        SOURCES "${tests_dir}/test_service.cpp"
        DEPENDENCIES cy::save)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_save_declare_tests)
