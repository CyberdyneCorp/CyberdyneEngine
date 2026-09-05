# src/servers/audio/tests/ — the audio server's suites. Tasks 4.3.4 and 4.3.5.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred and the taxonomy is read back out of tests/ rather than restated, because a budget
# copied into a second file is a budget that drifts. src/servers/render/ and src/servers/camera/ do
# the same.
#
# TWO SUITES, SPLIT BY WHAT THEY COST RATHER THAN BY WHAT THEY COVER:
#
#   audio_graph   the bus graph, the spatialisation functions, the command ring and the null
#                 backend. All pure or nearly so, all well inside the unit budget.
#   audio_mixer   the server, which MIXES REAL AUDIO — a case that plays a second of sound through
#                 the null backend does hundreds of blocks of arithmetic, and that is an integration
#                 cost rather than a unit one. Putting it under the unit budget would make the
#                 suite's own timing the thing most likely to fail.
#
# NEITHER NEEDS A DEVICE. The null backend is driven rather than free-running, so the mixer runs to a
# frame count a case chooses and every assertion is on samples. That is what lets `just test-all` run
# the whole of audio on a machine with no sound card — and what makes the null backend permanent
# rather than scaffolding (design.md §4).
set_property(GLOBAL PROPERTY CY_AUDIO_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_audio_declare_tests)
    get_property(audio_tests_dir GLOBAL PROPERTY CY_AUDIO_TESTS_DIR)

    foreach(kind unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/servers/audio/tests: the '${kind}' entry of the test taxonomy could not be "
                "read from tests/CMakeLists.txt. If the budget variables were renamed, this lookup "
                "and the deferral around it should go away — declare the suites from tests/ "
                "instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME audio_graph
        KIND unit
        SOURCES
            "${audio_tests_dir}/test_backend.cpp"
            "${audio_tests_dir}/test_bus.cpp"
            "${audio_tests_dir}/test_commands.cpp"
            "${audio_tests_dir}/test_spatial.cpp"
        DEPENDENCIES cy::servers-audio)

    cy_add_test(
        NAME audio_mixer
        KIND integration
        SOURCES "${audio_tests_dir}/test_server.cpp"
        DEPENDENCIES cy::servers-audio)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_audio_declare_tests)
