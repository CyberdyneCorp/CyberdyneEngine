# src/graph/material/tests/ — the material lowering's suite. M11.c task 6.1a.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing, and the taxonomy is read back out
# of tests/ rather than restated, exactly as src/rendering/material/tests/ does it.
#
#   graph_material  the palette against the compiler's own ops, the pins against the compiler's
#                   ports, and the authored canvas against BOTH of M7's front ends. Unit: nothing
#                   opens a file or a device, and the reference material is twenty-nine nodes
#                   compiled with `derive_family` left at its default — which is what M7's own
#                   `material_compiler` suite does at unit cost.
#
# IT INCLUDES `src/rendering/material/tests/` ON PURPOSE. The authored canvas is compared against THE
# fixture M7 wrote rather than against a copy of it: `tools/editor/selftest.py`'s rule, for the reason
# it gives — a copied fixture goes stale, and a stale fixture agrees with a broken check.
set_property(GLOBAL PROPERTY CY_GRAPH_MATERIAL_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_graph_material_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_GRAPH_MATERIAL_TESTS_DIR)

    foreach(kind IN ITEMS unit)
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

    target_include_directories(cy_test_unit_graph_material
                               PRIVATE "${CMAKE_SOURCE_DIR}/src/rendering/material/tests")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_graph_material_declare_tests)
