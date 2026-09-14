# src/core/plugins/tests/ — the plugin surface's two suites.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ after src/, so that function does not exist while this file is read. The declaration
# is therefore deferred, exactly as src/core/config/tests/ and src/scene/serialization/tests/ do it,
# and for the reason those files record: a deferred add_subdirectory() is rejected by CMake, so the
# deferral has to be of the declaration.
#
# THE SPLIT IS THE TAXONOMY'S AND IT IS ABOUT WHAT A CASE DOES.
#
#   unit.project        a manifest, a version, a constraint, a resolution, a lockfile, an extension
#                       point. Every case is a handful of objects and no sequence.
#   integration.plugins the LIFECYCLE: a set of plugins taken up through four phases each and back
#                       down through four more, with a failure contained in the middle of it. That
#                       is a sequence over several objects, which is what the integration row of the
#                       taxonomy is for.

set_property(GLOBAL PROPERTY CY_PLUGINS_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_plugins_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_PLUGINS_TESTS_DIR)

    # The taxonomy is read back from the directory that declares it rather than restated here: a
    # budget copied into a second file is a budget that drifts.
    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/core/plugins/tests: the '${kind}' entry of the test taxonomy could not be "
                "read from tests/CMakeLists.txt. If the budget variables were renamed, this lookup "
                "and the deferral around it should go away — declare the suites from tests/unit/ "
                "instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME project
        KIND unit
        SOURCES "${tests_dir}/test_project.cpp"
        DEPENDENCIES cy::core-plugins)

    cy_add_test(
        NAME plugins
        KIND integration
        SOURCES "${tests_dir}/test_lifecycle.cpp"
        DEPENDENCIES cy::core-plugins)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_plugins_declare_tests)
