# src/gameplay/tests/ — CyberGameplay's suites. Section 4.4.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory's processing and
# reading the taxonomy back out of tests/, exactly as src/servers/render/tests/ does — a budget
# copied into a second file is a budget that drifts.
#
#   gameplay_core   every case at unit cost. No world, no renderer, no audio device, no input
#                   server, and no GPU. `gameplay-framework`'s "Headless operation" is a
#                   requirement rather than a build configuration, and these suites are what
#                   keeping it looks like: they cannot reach a device because this module links
#                   nothing that has one.
#
# THE FILE TO READ FIRST IS test_bypass.cpp. It is task 4.4.6 and design.md §3's teeth: three tests
# at three depths — the include path cannot see an input header, the context exposes no input, and a
# simulation that reads around the stream replays *wrong* while looking perfectly correct as it
# runs. The `DEPENDENCIES` line below is load-bearing for the first of those: adding
# cy::servers-input here would put the input headers on this suite's include path and turn the
# `__has_include` check red. That is intended. Do not "fix" it by deleting the check.
set_property(GLOBAL PROPERTY CY_GAMEPLAY_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_gameplay_declare_tests)
    get_property(cy_gameplay_tests_dir GLOBAL PROPERTY CY_GAMEPLAY_TESTS_DIR)

    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_unit)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_unit)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/gameplay/tests: the 'unit' entry of the test taxonomy could not be read from "
            "tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
            "deferral around it should go away — declare the suites from tests/unit/ instead.")
    endif()
    set(CY_TEST_BUDGET_unit "${budget}")
    set(CY_TEST_TIMEOUT_unit "${timeout}")

    cy_add_test(
        NAME gameplay_core
        KIND unit
        SOURCES
            "${cy_gameplay_tests_dir}/test_bypass.cpp"
            "${cy_gameplay_tests_dir}/test_commands.cpp"
            "${cy_gameplay_tests_dir}/test_context.cpp"
            "${cy_gameplay_tests_dir}/test_control.cpp"
            "${cy_gameplay_tests_dir}/test_random.cpp"
            "${cy_gameplay_tests_dir}/test_validation.cpp"
        DEPENDENCIES cy::gameplay)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_gameplay_declare_tests)
