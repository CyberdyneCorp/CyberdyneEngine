# src/world/tests/ — CyberWorld's suites. Section 3 of the M6 tasks.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred to the end of the top-level directory's processing, and the taxonomy is read
# back out of tests/ rather than restated, because a budget copied into a second file is a budget
# that drifts. src/ecs/tests/ does the same, for the same reason.
#
# THE SPLIT BETWEEN unit AND integration IS ABOUT COST, AND HERE IT IS ALSO ABOUT WHAT IS BEING
# TESTED. A unit test has a millisecond, does no I/O and starts no thread.
#
#   world               the model, with no ECS world published into: coordinates and cell identity,
#                       the partitioner and spatial binding, cooked cells and their validation,
#                       streaming sources and their shapes, layers, HLOD and representation tiers,
#                       and the persistence overlay. Every case is tens of cells at most.
#   world_activation    publishes into a real `cy::ecs::World`: staged preparation across frames,
#                       atomic publication, layer switching over twenty thousand rows, the overlay
#                       applied during activation, and TEARDOWN MID-PREPARATION. Thousands of
#                       entities, which is over the unit budget by design — shrinking it would be
#                       testing a different claim than "activation is a bulk copy".
#   world_streaming     the whole tick: channels, budgets, priorities, eviction, diagnostics, the
#                       dependency closure, and a world destroyed mid-activation twelve times over.
#   world_traversal     task 3.8's exit criterion. Four hundred cells, two hundred and forty ticks,
#                       and the per-tick distribution reported beside its threshold. It is its own
#                       suite so that `ctest -R world_traversal` is the measurement on its own.

set_property(GLOBAL PROPERTY CY_WORLD_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_world_declare_tests)
    get_property(cy_world_tests_dir GLOBAL PROPERTY CY_WORLD_TESTS_DIR)

    # cy_add_test() reads the taxonomy from variables that tests/CMakeLists.txt sets in its own
    # directory scope, and this function runs in the top-level scope, which is not a child of it.
    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/world/tests: the '${kind}' entry of the test taxonomy could not be read from "
                "tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
                "deferral around it should go away — declare the suites from tests/unit/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME world
        KIND unit
        SOURCES
            "${cy_world_tests_dir}/test_cells.cpp"
            "${cy_world_tests_dir}/test_layers.cpp"
            "${cy_world_tests_dir}/test_overlay.cpp"
            "${cy_world_tests_dir}/test_partition.cpp"
            "${cy_world_tests_dir}/test_sources.cpp"
        DEPENDENCIES cy::world cy::ecs cy::core-reflect)

    # cy::core-diagnostics is here for one case: "publishing a cell leaves a level-transition
    # breadcrumb naming the cell" reads the breadcrumb ring back. `cy::world` links it privately, so
    # the headers are not on this target's include path without saying so.
    cy_add_test(
        NAME world_activation
        KIND integration
        SOURCES "${cy_world_tests_dir}/test_activation.cpp"
        DEPENDENCIES cy::world cy::ecs cy::core-reflect cy::core-diagnostics)

    cy_add_test(
        NAME world_streaming
        KIND integration
        SOURCES "${cy_world_tests_dir}/test_streaming.cpp"
        DEPENDENCIES cy::world cy::ecs cy::core-reflect)

    cy_add_test(
        NAME world_traversal
        KIND integration
        SOURCES "${cy_world_tests_dir}/test_traversal.cpp"
        DEPENDENCIES cy::world cy::ecs cy::core-reflect)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_world_declare_tests)
