# src/backends/audio-miniaudio/tests/ — the miniaudio backend's suite. Task 4.3.4.
#
# `integration`, not `unit`: it opens a real device and waits for the device's own thread to ask for
# a buffer, which is exactly the two things `testing-and-quality` puts outside the unit budget.
#
# NOT DEVICE-GATED. The suite's own header comment says why at length: both outcomes are asserted —
# a host with an audio system opens a device, and a host without one fails with `Unavailable` and
# nothing else. A suite that skipped would be green on precisely the machines where this backend is
# least exercised.
set_property(GLOBAL PROPERTY CY_MINIAUDIO_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_miniaudio_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_MINIAUDIO_TESTS_DIR)

    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_integration)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_integration)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/backends/audio-miniaudio/tests: the 'integration' entry of the test taxonomy "
            "could not be read from tests/CMakeLists.txt.")
    endif()
    set(CY_TEST_BUDGET_integration "${budget}")
    set(CY_TEST_TIMEOUT_integration "${timeout}")

    cy_add_test(
        NAME audio_miniaudio
        KIND integration
        SOURCES "${tests_dir}/test_miniaudio.cpp"
        DEPENDENCIES cy::backends-audio-miniaudio cy::servers-audio)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_miniaudio_declare_tests)
