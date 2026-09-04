# src/ecs/tests/ — the ECS's suites. Section 2 of the M2 tasks.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred to the end of the top-level directory's processing, by which time the
# taxonomy has been declared; the taxonomy itself is read back out of tests/ rather than restated,
# because a budget copied into a second file is a budget that drifts. src/core/memory/,
# src/core/jobs/, src/core/reflect/, src/core/values/, src/core/config/ and src/runtime/ all do the
# same, for the same reason.
#
# THE SPLIT BETWEEN unit AND integration IS THE TAXONOMY'S, AND IT IS ABOUT COST. A unit test has a
# millisecond, does no I/O and starts no thread.
#
#   ecs                 the model: entities, components, archetypes, queries, deferral, resources,
#                       relationships, systems and multiple worlds. Every case is a few hundred
#                       entities at most.
#   ecs_scale           bulk creation, bulk instantiation and chunk accounting. 100 000 entities is
#                       the number `ecs-core`'s "bulk creation is cheap" and "bulk instantiation"
#                       scenarios are stated at; shrinking it to fit a millisecond would be testing
#                       a different claim. The thousand-entity cases are here for the same reason
#                       and not a weaker one — they were measured at 1.0-2.1 ms in the Debug and
#                       Profile configurations, which is over the unit budget.
#   ecs_serialization   snapshots and the world byte stream. Larger worlds and several round trips.
#   ecs_scheduling      starts worker threads: it runs one schedule on the real job system and again
#                       serially, and asserts the two produce the same world.
#   ecs_diagnostics     opens the one trace and writes a file.

set_property(GLOBAL PROPERTY CY_ECS_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_ecs_declare_tests)
    get_property(cy_ecs_tests_dir GLOBAL PROPERTY CY_ECS_TESTS_DIR)

    # cy_add_test() reads the taxonomy from variables that tests/CMakeLists.txt sets in its own
    # directory scope, and this function runs in the top-level scope, which is not a child of it.
    # They are read back from the directory that defines them rather than restated here.
    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/ecs/tests: the '${kind}' entry of the test taxonomy could not be read from "
                "tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
                "deferral around it should go away — declare the suites from tests/unit/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME ecs
        KIND unit
        SOURCES
            "${cy_ecs_tests_dir}/test_archetypes.cpp"
            "${cy_ecs_tests_dir}/test_components.cpp"
            "${cy_ecs_tests_dir}/test_deferral.cpp"
            "${cy_ecs_tests_dir}/test_entities.cpp"
            "${cy_ecs_tests_dir}/test_queries.cpp"
            "${cy_ecs_tests_dir}/test_relationships.cpp"
            "${cy_ecs_tests_dir}/test_resources.cpp"
            "${cy_ecs_tests_dir}/test_systems.cpp"
            "${cy_ecs_tests_dir}/test_worlds.cpp"
        DEPENDENCIES cy::ecs cy::core-reflect)

    cy_add_test(
        NAME ecs_scale
        KIND integration
        SOURCES "${cy_ecs_tests_dir}/test_scale.cpp"
        DEPENDENCIES cy::ecs)

    cy_add_test(
        NAME ecs_serialization
        KIND integration
        SOURCES "${cy_ecs_tests_dir}/test_serialization.cpp"
        DEPENDENCIES cy::ecs)

    cy_add_test(
        NAME ecs_scheduling
        KIND integration
        SOURCES "${cy_ecs_tests_dir}/test_scheduling.cpp"
        DEPENDENCIES cy::ecs cy::core-jobs)

    # Links cy::core-diagnostics as well: the suite opens the one trace and reads back its
    # statistics, which is the point — the ECS emits onto the trace that already exists rather than
    # onto one of its own.
    cy_add_test(
        NAME ecs_diagnostics
        KIND integration
        SOURCES "${cy_ecs_tests_dir}/test_diagnostics.cpp"
        DEPENDENCIES cy::ecs cy::core-diagnostics)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_ecs_declare_tests)
