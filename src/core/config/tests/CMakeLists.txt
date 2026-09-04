# src/core/config/tests/ — the project graph and layered configuration. Section 4.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred: this file defines one function and asks CMake to call it at the end of the
# top-level directory's processing, by which time the taxonomy has been declared. (A deferred
# add_subdirectory() is rejected outright by CMake — "Subdirectories may not be created during
# deferred execution" — so the deferral has to be of the declaration, not of the directory.)
#
# src/core/reflect/, src/core/values/, src/core/memory/ and src/core/jobs/ all hit the same wall and
# solved it the same way. Five copies of one workaround: the fix is to add tests/ before src/ at the
# top level, or to lift the taxonomy into a cmake/ module both sides include. Recorded here rather
# than fixed here because the top-level CMakeLists.txt is not this workstream's file.
#
# Sources are absolute, because the deferred call runs in the top-level directory's scope and a
# relative path would be resolved against that directory rather than against this one.

set_property(GLOBAL PROPERTY CY_CONFIG_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_config_declare_tests)
    get_property(cy_config_tests_dir GLOBAL PROPERTY CY_CONFIG_TESTS_DIR)

    # The taxonomy is read back from the directory that declares it rather than restated here: a
    # budget copied into a second file is a budget that drifts.
    foreach(kind IN ITEMS unit)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/core/config/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
                "the deferral around it should go away — declare the suites directly from "
                "tests/unit/config/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME config
        KIND unit
        SOURCES
            "${cy_config_tests_dir}/test_module_registry.cpp"
            "${cy_config_tests_dir}/test_project.cpp"
            "${cy_config_tests_dir}/test_settings.cpp"
        DEPENDENCIES cy::core-config)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_config_declare_tests)

# The project graph's *rejections* — a cycle, an undeclared dependency, an upward layer dependency, a
# shipping target that reaches editor code — are configure-time failures over fixture projects, so
# they cannot be a C++ test: a program that links only exists because the graph was accepted.
# tools/project/selftest.py runs the fixtures and asserts both the exit status and that the
# diagnostic names both ends of the collision. Registered here so `ctest` runs it, and so a check
# that has silently stopped firing is a red build rather than a recipe nobody remembered.
find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)

add_test(NAME integration.project_graph
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/project/selftest.py")
set_tests_properties(integration.project_graph PROPERTIES
    TIMEOUT 300 LABELS integration)
