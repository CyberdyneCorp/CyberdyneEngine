# src/rendering/arbiter/tests/ — section 10 of the M7 tasks.
#
#   render_arbiter        unit. What the arbiter refuses, what its actuator does, where the deadband
#                         comes from, that a controller cannot relax on its own authority, and that
#                         pinning is total. Every case is a single decision, so every case is cheap.
#   render_arbiter_sweep  integration, and it is the CRITERION of task 10.1. 71 step loads x 1,400
#                         frames x 7 subsystems does not fit a 1 ms unit budget, and a sweep
#                         shortened to fit one would certify something other than the criterion —
#                         `design.md` §2.1 is explicit that one step magnitude can make any control
#                         law over a discrete ladder look stable.
set_property(GLOBAL PROPERTY CY_RENDER_ARBITER_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_render_arbiter_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_RENDER_ARBITER_TESTS_DIR)

    foreach(kind unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/arbiter/tests: the '${kind}' entry of the test taxonomy could not "
                "be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME render_arbiter
        KIND unit
        SOURCES
            "${tests_dir}/test_arbiter.cpp"
            "${tests_dir}/test_configuration.cpp"
            "${tests_dir}/test_profiles.cpp"
        DEPENDENCIES cy::rendering-arbiter)

    cy_add_test(
        NAME render_arbiter_sweep
        KIND integration
        SOURCES "${tests_dir}/test_sweep.cpp"
        DEPENDENCIES cy::rendering-arbiter)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_render_arbiter_declare_tests)
