# src/audio/tests/ — the acoustics half's suite. M8.b tasks 10.1 and 10.2.
#
# ONE SUITE, AND IT IS UNIT. No device is opened and no callback runs: the geometry cache is driven
# by a test extractor, the tier assignment is arithmetic over an array, the effect chain processes a
# block of samples the test wrote, and the transitions are arithmetic on a sample count.

set_property(GLOBAL PROPERTY CY_AUDIO_ACOUSTICS_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_audio_acoustics_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_AUDIO_ACOUSTICS_TESTS_DIR)

    foreach(kind IN ITEMS unit)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR "src/audio/tests: the '${kind}' taxonomy entry is missing.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME audio_acoustics
        KIND unit
        SOURCES
            "${tests_dir}/test_acoustics.cpp"
            "${tests_dir}/test_tiers.cpp"
            "${tests_dir}/test_effects.cpp"
            "${tests_dir}/test_interactive.cpp"
        DEPENDENCIES cy::audio-acoustics)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_audio_acoustics_declare_tests)
