# src/navigation/tests/ — the navigation stack's suites. M8.b section 6.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing and the taxonomy is read back out
# of tests/ rather than restated, the same way src/world/tests/ and src/graph/tests/ do it.
#
#   navigation      the mesh, the queries and the fields: tiles published and released, adjacency
#                   within and across a border, stale references, obstacles, off-mesh links, A*, the
#                   funnel, budgets and partial paths, the deterministic async queue, flow-field
#                   generation and its incremental regeneration, the region hierarchy, and local
#                   avoidance, the components, and streaming — tiles and regions in and out, an
#                   agent repathing under one, and navigation's tiling staying its own. Unit: every
#                   case builds a mesh of a few dozen polygons.
#
#   navigation_fields
#                   flow fields: generation, determinism, incremental regeneration and the cache.
#                   INTEGRATION — one case integrates a Dijkstra field over a hundred and
#                   twenty-eight cells twice and compares every cell of the two, which is over the
#                   unit tier's millisecond by construction rather than by accident.
#
#   navigation_build
#                   tile generation over both back ends: erosion by the agent radius, the slope
#                   limit, the declared filters, and the refusals. INTEGRATION — it voxelises.
#
#   navigation_crowd
#                   the crowd at scale, and the exit criterion the milestone names. INTEGRATION, and
#                   deliberately: the subject IS eight thousand agents stepped for sixty ticks, so
#                   making it cheap would mean measuring something else. Hard rule 7.
set_property(GLOBAL PROPERTY CY_NAVIGATION_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_navigation_declare_tests)
    get_property(cy_navigation_tests_dir GLOBAL PROPERTY CY_NAVIGATION_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/navigation/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME navigation
        KIND unit
        SOURCES
            "${cy_navigation_tests_dir}/test_components.cpp"
            "${cy_navigation_tests_dir}/test_hierarchy.cpp"
            "${cy_navigation_tests_dir}/test_streaming.cpp"
            "${cy_navigation_tests_dir}/test_navmesh.cpp"
            "${cy_navigation_tests_dir}/test_query.cpp"
        DEPENDENCIES cy::navigation cy::ecs cy::core-reflect)

    cy_add_test(
        NAME navigation_build
        KIND integration
        SOURCES "${cy_navigation_tests_dir}/test_build.cpp"
        DEPENDENCIES cy::navigation)

    cy_add_test(
        NAME navigation_crowd
        KIND integration
        SOURCES "${cy_navigation_tests_dir}/test_crowd.cpp"
        DEPENDENCIES cy::navigation)

    cy_add_test(
        NAME navigation_fields
        KIND integration
        SOURCES "${cy_navigation_tests_dir}/test_flow_field.cpp"
        DEPENDENCIES cy::navigation)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_navigation_declare_tests)
