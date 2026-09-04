# src/runtime/tests/ — the startup sequence, the tick loop, backend selection and feature slicing.
# Tasks 4.5 (M1) and 4.1.x / 4.2.x (M2).
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing, by which time the taxonomy has been
# declared; the taxonomy itself is read back out of tests/ rather than restated, because a budget
# copied into a second file is a budget that drifts. src/core/reflect/, values/, memory/, jobs/,
# config/ and determinism/ all do the same, for the same reason.
#
# THREE KINDS, AND THE REASON FOR EACH:
#
#   unit         Backend selection and the feature table. Both are pure data — no world, no
#               threads, no processes — and run in microseconds.
#   integration  The tick pipeline over a real `World` and `SceneTree`. Every case allocates at
#               least one 16 KiB chunk and most bring up the scene tree's twelve components and
#               twenty-three templates; measured at 0.4-2.6 ms per case in Debug, against a unit
#               budget of 1 ms.
#   smoke        The two claims that are only makeable across *processes*: the module registration
#               order (M1) and the state hash's reproducibility (M2). A fresh address space each run
#               is what would expose a result depending on an allocation address, a per-process hash
#               seed or static initialisation order; iterations of one loop would reproduce such a
#               result faithfully and call it deterministic.

set_property(GLOBAL PROPERTY CY_RUNTIME_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_runtime_declare_tests)
    get_property(cy_runtime_tests_dir GLOBAL PROPERTY CY_RUNTIME_TESTS_DIR)

    foreach(kind IN ITEMS unit integration smoke)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/runtime/tests: the '${kind}' entry of the test taxonomy could not be read from "
                "tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
                "deferral around it should go away — declare the suite from tests/smoke/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME runtime
        KIND unit
        SOURCES
            "${cy_runtime_tests_dir}/test_features.cpp"
            "${cy_runtime_tests_dir}/test_servers.cpp"
        DEPENDENCIES cy::runtime)

    cy_add_test(
        NAME runtime_simulation
        KIND integration
        SOURCES "${cy_runtime_tests_dir}/test_simulation.cpp"
        DEPENDENCIES cy::runtime)

    cy_add_test(
        NAME module_order
        KIND smoke
        SOURCES "${cy_runtime_tests_dir}/test_module_order.cpp"
        DEFINITIONS CY_STARTUP_PROBE="$<TARGET_FILE:cy_runtime_startup_probe>")

    cy_add_test(
        NAME tick_loop
        KIND smoke
        SOURCES "${cy_runtime_tests_dir}/test_tick_loop.cpp"
        DEFINITIONS CY_TICK_LOOP_PROBE="$<TARGET_FILE:cy_runtime_tick_loop_probe>")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_runtime_declare_tests)
