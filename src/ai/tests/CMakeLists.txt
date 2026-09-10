# src/ai/tests/ — CyberAI's suites. M8.b section 6.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing and the taxonomy is read back out
# of tests/ rather than restated, the same way src/navigation/tests/ and src/graph/tests/ do it.
#
#   ai              the model: the knowledge store and its decay, the environment query's
#                   generators, tests, budget and ranking, and smart objects with their
#                   reservations. Unit: no world, no program, a few dozen candidates per case.
#
#   ai_runtime      the runtime over a real `cy::ecs::World` and a real compiled behaviour program:
#                   components, the tier policy, the deterministic rotation, the budget and its
#                   deferral, the decision history, and batched perception end to end. INTEGRATION —
#                   it builds a world and compiles a graph.
#
#   ai_scale        the milestone's exit criterion: eight thousand agents thinking for a hundred and
#                   twenty ticks, with the per-agent cost reported beside its threshold, and the
#                   teardown-under-load case. INTEGRATION, and its own suite so that
#                   `ctest -R integration.ai_scale` is the measurement on its own.
set_property(GLOBAL PROPERTY CY_AI_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_ai_declare_tests)
    get_property(cy_ai_tests_dir GLOBAL PROPERTY CY_AI_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/ai/tests: the '${kind}' entry of the test taxonomy could not be read from "
                "tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME ai
        KIND unit
        SOURCES
            "${cy_ai_tests_dir}/test_environment_query.cpp"
            "${cy_ai_tests_dir}/test_knowledge.cpp"
            "${cy_ai_tests_dir}/test_smart_object.cpp"
        DEPENDENCIES cy::ai)

    cy_add_test(
        NAME ai_runtime
        KIND integration
        SOURCES
            "${cy_ai_tests_dir}/test_perception.cpp"
            "${cy_ai_tests_dir}/test_runtime.cpp"
        DEPENDENCIES cy::ai cy::ecs cy::core-reflect)

    cy_add_test(
        NAME ai_scale
        KIND integration
        SOURCES "${cy_ai_tests_dir}/test_scale.cpp"
        DEPENDENCIES cy::ai cy::ecs cy::core-reflect)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_ai_declare_tests)
