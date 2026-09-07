# tools/build/tests/ — the derivation graph's suites. M6 section 7.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* tools/, so that function does not exist while this file is read. The
# declaration is therefore deferred to the end of the top-level directory's processing, and the
# taxonomy is read back out of tests/ rather than restated. tools/cook/tests/ and
# tools/import/tests/ do the same, for the same reason.
#
# TWO KINDS, AND THE SPLIT IS NOT ARBITRARY. `unit.build_graph` is the part that touches no
# filesystem: topology, keys, the text formats. Everything else writes real files and destroys
# services under load, which is what the properties are about — a cache that answers the second run,
# a patch interrupted by a process that is killed — and shrinking those to fit a millisecond would
# be testing something smaller than the build.

set_property(GLOBAL PROPERTY CY_BUILD_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_build_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_BUILD_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "tools/build/tests: the '${kind}' entry of the test taxonomy could not be read from "
                "tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
                "deferral around it should go away — declare the suite from tests/integration/ "
                "instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME build_graph
        KIND unit
        SOURCES "${tests_dir}/test_build_graph.cpp"
        DEPENDENCIES cy::build-graph)

    cy_add_test(
        NAME build_service
        KIND integration
        SOURCES "${tests_dir}/test_build_service.cpp"
        DEPENDENCIES cy::build-graph)

    cy_add_test(
        NAME build_patch
        KIND integration
        SOURCES "${tests_dir}/test_build_patch.cpp"
        DEPENDENCIES cy::build-graph)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_build_declare_tests)
