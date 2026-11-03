# tools/ship/tests/ — the signed manifest, its startup check and a game's launch. Issue #87.
#
# Deferred like every suite declared from tools/, because `cy_add_test` is defined in tests/ and the
# top-level directory adds tests/ after tools/.
#
# `integration`: every case writes and reads a real installation on disk and signs or verifies it,
# which is milliseconds rather than the unit kind's one. smoke.ship_games, beside it, packages the
# real games and launches them from a temporary directory.

set_property(GLOBAL PROPERTY CY_SHIP_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_ship_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_SHIP_TESTS_DIR)
    foreach(kind IN ITEMS integration smoke)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "tools/ship/tests: the '${kind}' entry of the test taxonomy could not be read from "
                "tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME ship_manifest
        KIND integration
        SOURCES "${tests_dir}/test_ship.cpp"
        DEPENDENCIES cy::ship)

    # smoke.ship_games: `just package` for every game this tree built, each artefact unpacked into
    # a temporary directory with no source tree beneath it and launched there — the scripted run
    # held to the build tree's digests, a tampered chunk refused, a crash reported. tools/ship/
    # test_ship_games.py says what each act checks.
    find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
    if(NOT Python3_Interpreter_FOUND)
        message(STATUS "tools/ship: python3 is absent, so smoke.ship_games is not declared.")
        return()
    endif()
    add_test(NAME smoke.ship_games
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/ship/test_ship_games.py"
                     --build-dir "${CMAKE_BINARY_DIR}"
                     --work "${CMAKE_CURRENT_BINARY_DIR}/ship-games"
             WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(smoke.ship_games PROPERTIES
                         LABELS "smoke" TIMEOUT "${CY_TEST_TIMEOUT_smoke}"
                         SKIP_RETURN_CODE 77)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_ship_declare_tests)
