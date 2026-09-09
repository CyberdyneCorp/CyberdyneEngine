# src/graph/tests/ — CyberGraph's suites. M8.b section 2.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing and the taxonomy is read back out
# of tests/ rather than restated, the same way src/rendering/material/tests/ does it.
#
#   cybergraph   the authoring layer: identity, typed pins, the textual source round trip, semantic
#                diff and three-way merge, migration, opaque preservation, diagnostics and the
#                determinism audit. Unit: every case builds a graph of a dozen nodes and touches no
#                file, no device and no world.
#
#   graph_compiler
#                the expression core, the anchor, and the four consumer lowerings. INTEGRATION, and
#                deliberately: a case here runs the optimisation pipeline to a fixed point over a
#                26-node material several times and emits its program, and the material compiler's
#                own suite learnt at M7 that exactly this shape does not fit the unit tier's
#                millisecond in a Debug configuration. The subject IS the repeated compilation, so
#                making it cheaper would mean measuring something else.
set_property(GLOBAL PROPERTY CY_GRAPH_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_graph_declare_tests)
    get_property(cy_graph_tests_dir GLOBAL PROPERTY CY_GRAPH_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/graph/tests: the '${kind}' entry of the test taxonomy could not be read from "
                "tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
                "deferral around it should go away — declare the suites from tests/unit/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME cybergraph
        KIND unit
        SOURCES
            "${cy_graph_tests_dir}/test_cybergraph.cpp"
            "${cy_graph_tests_dir}/test_merge.cpp"
            "${cy_graph_tests_dir}/test_audit.cpp"
        DEPENDENCIES cy::graph)

    cy_add_test(
        NAME graph_compiler
        KIND integration
        SOURCES
            "${cy_graph_tests_dir}/anchor/material_anchor.cpp"
            "${cy_graph_tests_dir}/anchor/material_anchor_emit.cpp"
            "${cy_graph_tests_dir}/anchor/material_anchor_graph.cpp"
            "${cy_graph_tests_dir}/test_anchor.cpp"
            "${cy_graph_tests_dir}/test_expr.cpp"
            "${cy_graph_tests_dir}/test_lowering.cpp"
        DEPENDENCIES cy::graph)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_graph_declare_tests)
