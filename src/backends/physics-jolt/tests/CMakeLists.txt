# src/backends/physics-jolt/tests/ — the Jolt backend's suite. Task 4.2.2.
#
# `integration`, not `unit`. Every case here builds a world, drops bodies into it and steps for one
# to three seconds of simulated time, which is what it takes to observe a contact being RESOLVED —
# the one thing this backend has that the reference backend does not. A unit budget of one
# millisecond of CPU per case would be met by trimming the cases until they stopped measuring that.
#
# THE TARGET LINKS `cy::physics-jolt` AND NOT `cy::dep::jolt`. That is the requirement being checked
# by the build: `physics` — "no Jolt type SHALL appear in any header outside the backend module", and
# a case here that tried to name one would not compile, because Jolt's include directories are a
# PRIVATE usage requirement of the backend and are not inherited.
set_property(GLOBAL PROPERTY CY_PHYSICS_JOLT_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_physics_jolt_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_PHYSICS_JOLT_TESTS_DIR)
    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_integration)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_integration)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/backends/physics-jolt/tests: the 'integration' entry of the test taxonomy could "
            "not be read from tests/CMakeLists.txt.")
    endif()
    set(CY_TEST_BUDGET_integration "${budget}")
    set(CY_TEST_TIMEOUT_integration "${timeout}")

    cy_add_test(
        NAME physics_jolt
        KIND integration
        SOURCES "${tests_dir}/test_jolt.cpp"
        DEPENDENCIES cy::physics-jolt cy::core-jobs)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_physics_jolt_declare_tests)
