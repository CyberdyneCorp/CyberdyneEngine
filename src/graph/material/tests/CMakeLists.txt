# src/graph/material/tests/ — the material lowering's suite. M11.c task 6.1a.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing, and the taxonomy is read back out
# of tests/ rather than restated, exactly as src/rendering/material/tests/ does it.
#
# TWO SUITES, ONE FIXTURE, AND THE LINE BETWEEN THEM IS WHAT A CASE COSTS.
#
#   graph_material  KIND unit. The palette against the compiler's own ops, a node type this build
#                   cannot lower, and every input pin against the port the compiler reads — plus the
#                   authored canvas compared against M7's hand-built `MaterialGraph`, digest for
#                   digest. Nothing opens a file or a device and no case costs more than 0.107 ms of
#                   CPU against the tier's 1.000 ms.
#
#   graph_material  KIND integration. The one case that COMPILES the material twice — once from the
#                   canvas and once from the text definition — and compares the two cook keys.
#                   `tests/harness/src/budget.cpp` measured it at 0.806 ms of CPU, 81% of the unit
#                   tier on a single case, which is what made `unit.graph_material` fail the budget
#                   twice inside M11.c's closing ledger on a cold governor while passing every
#                   standalone run. The instrument's own message names the remedy — "the taxonomy in
#                   `testing-and-quality` places a test this expensive in the next suite up — move
#                   it, or make it cheaper" — and this is the move. Repeated compilation to a fixed
#                   point is the same shape src/graph/tests/ already records as not fitting the unit
#                   tier, and the same reason src/vfx/tests/ declares `vfx_compiler` at integration.
#                   THE ASSERTION DID NOT CHANGE IN THE MOVE: same two front ends, same material,
#                   same cook key. A budget raised until a case fitted under it would have been the
#                   other thing, and it is not what happened here.
#
# The fixture both suites author — the twenty-nine-node canvas — lives in reference_canvas.h and is
# INCLUDED by each rather than copied into the second, for the reason that header gives.
#
# IT INCLUDES `src/rendering/material/tests/` ON PURPOSE. The authored canvas is compared against THE
# fixture M7 wrote rather than against a copy of it: `tools/editor/selftest.py`'s rule, for the reason
# it gives — a copied fixture goes stale, and a stale fixture agrees with a broken check.
set_property(GLOBAL PROPERTY CY_GRAPH_MATERIAL_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_graph_material_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_GRAPH_MATERIAL_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/graph/material/tests: the '${kind}' entry of the test taxonomy could not be "
                "read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME graph_material
        KIND unit
        SOURCES "${tests_dir}/test_lower_material.cpp"
        DEPENDENCIES cy::graph-material)

    cy_add_test(
        NAME graph_material
        KIND integration
        SOURCES "${tests_dir}/test_material_front_ends.cpp"
        DEPENDENCIES cy::graph-material)

    # For "fixtures.h" — M7's own reference material, included rather than transcribed — and for
    # "reference_canvas.h", which sits beside this file.
    foreach(target cy_test_unit_graph_material cy_test_integration_graph_material)
        target_include_directories(
            ${target} PRIVATE "${CMAKE_SOURCE_DIR}/src/rendering/material/tests" "${tests_dir}")
    endforeach()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_graph_material_declare_tests)
