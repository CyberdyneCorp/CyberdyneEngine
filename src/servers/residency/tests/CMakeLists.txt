# src/servers/residency/tests/ — the residency layer's suites. M6 section 4.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing and the taxonomy is read back out
# of tests/ rather than restated, because a budget copied into a second file is a budget that
# drifts. src/servers/render/, src/servers/camera/, src/scene/ and src/ecs/ all do the same.
#
#   residency          unit. No thread, no allocation beyond the policy's own tables, no world and
#                      no device in any of them — the residency layer is layer 2 and owns policy
#                      rather than storage, which is what makes "a page is a subsystem tag and an
#                      opaque number" a property of the build rather than of a mock.
#
#                      test_separation.cpp is M6's exit criterion: **a test holds bytes resident
#                      with simulation off.** It is in the unit suite deliberately — the criterion
#                      is that residency needs nothing else to be running, and a case that had to
#                      stand up a world to prove it would be proving the opposite.
#
#   residency_teardown integration, because it spawns threads and joins them, and
#                      `testing-and-quality` places waiting above the unit tier. It tears the
#                      subsystem down while four workers are requesting, holding and reporting into
#                      it, two hundred times over, and destroys two hundred servers that still hold
#                      pages, holds, requests and deadlines. M5.5's gate found this project's first
#                      real engine defect exactly this way.
set_property(GLOBAL PROPERTY CY_RESIDENCY_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_residency_declare_tests)
    get_property(residency_tests_dir GLOBAL PROPERTY CY_RESIDENCY_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/servers/residency/tests: the '${kind}' entry of the test taxonomy could not "
                "be read from tests/CMakeLists.txt. If the budget variables were renamed, this "
                "lookup and the deferral around it should go away — declare the suites from "
                "tests/${kind}/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME residency
        KIND unit
        SOURCES
            "${residency_tests_dir}/test_deadline.cpp"
            "${residency_tests_dir}/test_importance.cpp"
            "${residency_tests_dir}/test_policy.cpp"
            "${residency_tests_dir}/test_request.cpp"
            "${residency_tests_dir}/test_separation.cpp"
            "${residency_tests_dir}/test_server.cpp"
        DEPENDENCIES cy::servers-residency)

    cy_add_test(
        NAME residency_teardown
        KIND integration
        SOURCES "${residency_tests_dir}/test_teardown.cpp"
        DEPENDENCIES cy::servers-residency)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_residency_declare_tests)
