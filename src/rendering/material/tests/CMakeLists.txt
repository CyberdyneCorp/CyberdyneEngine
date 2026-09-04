# src/rendering/material/tests/ — the material model's suites. Section 4.2 of the M3 tasks.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing, and the taxonomy is read back out
# of tests/ rather than restated, because a budget copied into a second file is a budget that
# drifts. src/servers/render/, src/scene/ and src/rendering/graph/ all do the same.
#
#   material      the BRDF's terms, the parameter layout, the table, the standard material, the
#                 fallbacks and validation — all at unit cost, because none of them allocates more
#                 than a few kilobytes and none of them integrates anything.
#
#   material_ibl  the parts of image-based lighting that cannot fit a millisecond. The DFG bake is
#                 `size² · samples` importance samples with a square root and two transcendentals
#                 each, and the 32² table at 128 samples that the determinism claim rests on is
#                 131 072 of them. The spherical-harmonic orthonormality case is here for the same
#                 reason and arrived later: it integrates 4 096 directions against a nine-term basis
#                 and measured up to 1.24 ms of CPU against the unit suite's 1 ms budget, which
#                 failed about one run in ten on an idle machine and took M0's ledger down with it.
#                 Shrinking either to fit the unit budget would mean widening a tolerance until the
#                 property it checks stopped being visible — which is testing something else.
#                 `testing-and-quality`'s taxonomy has a suite for exactly this.
set_property(GLOBAL PROPERTY CY_MATERIAL_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_material_declare_tests)
    get_property(cy_material_tests_dir GLOBAL PROPERTY CY_MATERIAL_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/material/tests: the '${kind}' entry of the test taxonomy could not "
                "be read from tests/CMakeLists.txt. If the budget variables were renamed, this "
                "lookup and the deferral around it should go away — declare the suites from "
                "tests/unit/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME material
        KIND unit
        SOURCES
            "${cy_material_tests_dir}/test_brdf.cpp"
            "${cy_material_tests_dir}/test_material.cpp"
            "${cy_material_tests_dir}/test_standard.cpp"
            "${cy_material_tests_dir}/test_validation.cpp"
        DEPENDENCIES cy::rendering-material)

    cy_add_test(
        NAME material_ibl
        KIND integration
        SOURCES "${cy_material_tests_dir}/test_ibl.cpp"
        DEPENDENCIES cy::rendering-material)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_material_declare_tests)
