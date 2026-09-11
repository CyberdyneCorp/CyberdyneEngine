# src/networking/tests/ — CyberNet's suites. M9 section 4.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory's processing and
# reading the taxonomy back out of tests/, exactly as src/gameplay/tests/ and src/replay/tests/ do:
# a budget copied into a second file is a budget that drifts.
#
#   networking_core     `unit`: arithmetic over modes, authority, the reliability windows, compiled
#                       schemas, RPC validation and the prediction policy. No world, no socket, no
#                       device, and no case that builds a session.
#   networking_session  `integration`: the transports under adverse conditions, baselines and
#                       deltas, interest over ten thousand entities, the scheduler's degradation
#                       order, the dedicated server, and teardown past every bound. Every case here
#                       builds a session or a population, and the taxonomy's rule is about cost:
#                       a case that builds one does not belong in a tier budgeted at one
#                       millisecond of CPU.
#   networking_udp      `integration`: a real UDP socket on the loopback. Separate from the rest so
#                       that a machine which refuses to bind a socket loses one suite rather than
#                       fourteen cases about arithmetic.
set_property(GLOBAL PROPERTY CY_NETWORKING_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_networking_declare_tests)
    get_property(cy_networking_tests_dir GLOBAL PROPERTY CY_NETWORKING_TESTS_DIR)

    foreach(kind unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/networking/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
                "the deferral around it should go away — declare the suites from tests/unit/ "
                "instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME networking_core
        KIND unit
        SOURCES
            "${cy_networking_tests_dir}/test_authority.cpp"
            "${cy_networking_tests_dir}/test_mode.cpp"
            "${cy_networking_tests_dir}/test_prediction.cpp"
            "${cy_networking_tests_dir}/test_reliability.cpp"
            "${cy_networking_tests_dir}/test_rpc.cpp"
            "${cy_networking_tests_dir}/test_schema.cpp"
        DEPENDENCIES cy::networking)

    cy_add_test(
        NAME networking_session
        KIND integration
        SOURCES
            "${cy_networking_tests_dir}/test_interest.cpp"
            "${cy_networking_tests_dir}/test_replication.cpp"
            "${cy_networking_tests_dir}/test_scheduler.cpp"
            "${cy_networking_tests_dir}/test_server.cpp"
            "${cy_networking_tests_dir}/test_teardown.cpp"
            "${cy_networking_tests_dir}/test_transport.cpp"
        DEPENDENCIES cy::networking)

    cy_add_test(
        NAME networking_udp
        KIND integration
        SOURCES "${cy_networking_tests_dir}/test_udp.cpp"
        DEPENDENCIES cy::networking)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_networking_declare_tests)
