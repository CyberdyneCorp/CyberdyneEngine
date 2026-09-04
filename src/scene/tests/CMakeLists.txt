# src/scene/tests/ — the node façade's suites. Section 3.1 of the M2 tasks.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred to the end of the top-level directory's processing, by which time the
# taxonomy has been declared; the taxonomy itself is read back out of tests/ rather than restated,
# because a budget copied into a second file is a budget that drifts. src/ecs/, src/core/memory/,
# src/core/jobs/, src/core/reflect/ and src/runtime/ all do the same, for the same reason.
#
#   scene         the model, at unit cost: nodes as views, hierarchy and naming, node types and
#                 templates, and the lifecycle callbacks. A few dozen nodes per case and no
#                 whole-tree walk.
#   scene_scale   the suites whose cases do not fit a millisecond, measured rather than guessed. A
#                 scene case is not cheap to begin with — every one builds a `World` and a
#                 `SceneTree` over it, which registers fourteen components — so the line between the
#                 two suites is drawn at what a case does *on top of* that fixture:
#
#                 test_behaviour.cpp   five hundred instances through one generated system — the
#                                      number `scene-graph-and-nodes`' scale scenario is stated at.
#                 test_transform.cpp   an eighty-one node hierarchy, so that "only that node's
#                                      subtree SHALL be recomputed" is a measurement and not a
#                                      restatement.
#                 test_coherence.cpp   every case walks the whole tree twice — once to check the
#                                      relation and once to recompute every world transform — which
#                                      is what a whole-tree invariant costs. Seven of its cases
#                                      measured 1.0-2.4 ms in the Debug configuration.
#                 test_groups.cpp      joining a group registers a tag component, which creates an
#                                      archetype and allocates a chunk; two of its three cases do it
#                                      twice, and both measured 1.015-1.096 ms in Debug.
#                 test_scenes.cpp      loads and unloads whole scene descriptions and ends on a
#                                      `coherent()` walk.
#                 test_visibility.cpp  ten `propagate()` calls and three `coherent()` walks across
#                                      its cases: the same whole-tree cost as test_coherence.cpp,
#                                      spread over more of them.
#
#                 Shrinking any of them to fit a millisecond would be testing something else.
#
# THE MEASUREMENT THIS SPLIT WAS DRAWN FROM, AND THE RESIDUAL. Taken at M2's close, in the Debug
# configuration, on a 24-core host, as `ctest -L unit` — which is how `just test-all` runs it and is
# a harsher condition than the binary alone, because seventeen suites run back to back:
#
#   with all seven files in the unit suite   16 failures in 30 runs
#   with these four moved to scene_scale      1 failure  in 30 runs
#
# The residual is `test_hierarchy.cpp`'s first case, which pays the process's cold start — the first
# chunk mapped, the first name interned, the first allocator domain built — measured at roughly
# 150 us on top of its own 85 us. Nothing here fixes that; it is a property of a per-case wall-clock
# budget at -O0, it is recorded in docs/roadmap/capability-matrix.md, and the honest fix is the
# harness's rather than this file's. Put a cheap case first and measure in Debug before adding to
# this suite.

set_property(GLOBAL PROPERTY CY_SCENE_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_scene_declare_tests)
    get_property(cy_scene_tests_dir GLOBAL PROPERTY CY_SCENE_TESTS_DIR)

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
                "src/scene/tests: the '${kind}' entry of the test taxonomy could not be read from "
                "tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
                "deferral around it should go away — declare the suites from tests/unit/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME scene
        KIND unit
        SOURCES
            "${cy_scene_tests_dir}/test_hierarchy.cpp"
            "${cy_scene_tests_dir}/test_lifecycle.cpp"
            "${cy_scene_tests_dir}/test_node.cpp"
            "${cy_scene_tests_dir}/test_templates.cpp"
        DEPENDENCIES cy::scene cy::core-reflect)

    cy_add_test(
        NAME scene_scale
        KIND integration
        SOURCES
            "${cy_scene_tests_dir}/test_behaviour.cpp"
            "${cy_scene_tests_dir}/test_coherence.cpp"
            "${cy_scene_tests_dir}/test_groups.cpp"
            "${cy_scene_tests_dir}/test_scenes.cpp"
            "${cy_scene_tests_dir}/test_transform.cpp"
            "${cy_scene_tests_dir}/test_visibility.cpp"
        DEPENDENCIES cy::scene cy::core-reflect)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_scene_declare_tests)
