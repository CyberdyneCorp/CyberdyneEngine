# src/environment/tests/ — CyberField's suites. M10 section 1.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred to the end of the top-level directory's processing, and the taxonomy is read
# back out of tests/ rather than restated, because a budget copied into a second file is a budget
# that drifts. src/world/tests/ and src/servers/residency/tests/ do the same, for the same reason.
#
# THE SPLIT IS ABOUT COST, AND HERE IT IS ALSO ABOUT WHAT IS BEING MEASURED.
#
#   environment            unit. The declaration's own rules, the refusal that names both producers,
#                          the sparse store and its two sampling paths, the layer rule, the change
#                          queue and the firewall. A millisecond each, no thread and no I/O.
#   environment_gpu        integration, because the CPU/GPU agreement is a MEASUREMENT over
#                          thousands of positions rather than an assertion at one of them, and
#                          thousands of samples is over the unit budget by design — shrinking it
#                          would be testing a different claim than "the two implementations agree".
#   environment_streaming  integration: a real `residency::ResidencyServer`, a real
#                          `world::CellEventQueue`, cells bound and released, admissions honoured
#                          and evictions refused for guaranteed tiles.

set_property(GLOBAL PROPERTY CY_ENVIRONMENT_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_environment_declare_tests)
    get_property(cy_environment_tests_dir GLOBAL PROPERTY CY_ENVIRONMENT_TESTS_DIR)

    # cy_add_test() reads the taxonomy from variables that tests/CMakeLists.txt sets in its own
    # directory scope, and this function runs in the top-level scope, which is not a child of it.
    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/environment/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
                "the deferral around it should go away — declare the suites from tests/unit/ "
                "instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME environment
        KIND unit
        SOURCES
            "${cy_environment_tests_dir}/test_declaration.cpp"
            "${cy_environment_tests_dir}/test_determinism.cpp"
            "${cy_environment_tests_dir}/test_producers.cpp"
            "${cy_environment_tests_dir}/test_store.cpp"
        DEPENDENCIES cy::environment)

    cy_add_test(
        NAME environment_gpu
        KIND integration
        SOURCES "${cy_environment_tests_dir}/test_gpu.cpp"
        DEPENDENCIES cy::environment)

    cy_add_test(
        NAME environment_streaming
        KIND integration
        SOURCES "${cy_environment_tests_dir}/test_streaming.cpp"
        DEPENDENCIES cy::environment)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_environment_declare_tests)
