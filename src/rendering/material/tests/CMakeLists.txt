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
#   material_compiler
#                 M7's compiler at unit cost: the IR and its content hash, the two front-ends, and
#                 the node preview. The spike measured a 24-node material at a median of 57 us from
#                 graph to generated Slang, and nothing in these three files compiles the family
#                 more than twice.
#
#   material_lowering
#                 The same compiler where a case compiles the whole PROGRAM FAMILY — twelve programs
#                 across four uses and three tiers — several times over and diffs the results.
#                 `material_ibl` below is in this file for exactly this reason and says so; these
#                 cases are the second instance and were found the same way, by M7's closing gate
#                 rather than by a person reading. Measured on this tree, Debug configuration, on a
#                 quiet machine: `a derivation that changes albedo is flagged` 2.26 ms, `every
#                 switch is load-bearing` 1.24 ms, `cost scales with programs, not with instances`
#                 1.23 ms and `an opaque material has no shadow program at all` 1.29 ms, each
#                 against the unit suite's 1 ms. In Development they are 0.35 to 0.62 ms, which is
#                 why `unit.material_compiler` was green in dev and red in Debug, and why it also
#                 failed whenever the machine was busy. Making them cheaper means compiling fewer
#                 programs, and the number of programs IS the property: `material-compiler` requires
#                 a derived program to be checked against the primary one it was derived from.
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

    # M7's material compiler (tasks 6.1-6.4). A suite of its own rather than more cases in
    # `material`, because the two subjects are different: `material` is the model and its table, and
    # this is the compiler that fills them in. Unit, because nothing here touches a device, a file or
    # a shader toolchain — the exit criterion "a graph and a hand-written material produce identical
    # programs" is a property of generated TEXT, which is the whole reason it is checkable at all.
    cy_add_test(
        NAME material_compiler
        KIND unit
        SOURCES
            "${cy_material_tests_dir}/test_ir.cpp"
            "${cy_material_tests_dir}/test_frontends.cpp"
            "${cy_material_tests_dir}/test_preview.cpp"
        DEPENDENCIES cy::rendering-material)

    # The same compiler, integration cost. Nothing about these two files is slower than it should
    # be — they are the cases that build the twelve-program family repeatedly, which is the subject
    # rather than an accident of how they are written — and `testing-and-quality`'s taxonomy has a
    # suite for exactly that. Splitting them out is what keeps `unit.material_compiler` inside its
    # budget in every configuration rather than only in the optimised ones.
    cy_add_test(
        NAME material_lowering
        KIND integration
        SOURCES
            "${cy_material_tests_dir}/test_passes.cpp"
            "${cy_material_tests_dir}/test_lowering.cpp"
        DEPENDENCIES cy::rendering-material)

    cy_add_test(
        NAME material_ibl
        KIND integration
        SOURCES "${cy_material_tests_dir}/test_ibl.cpp"
        DEPENDENCIES cy::rendering-material)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_material_declare_tests)
