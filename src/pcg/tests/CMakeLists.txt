# src/pcg/tests/ — CyberPCG's suites. M10 section 4.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred to the end of the top-level directory's processing, and the taxonomy is read
# back out of tests/ rather than restated, because a budget copied into a second file is a budget
# that drifts. src/environment/tests/ and src/world/tests/ do the same, for the same reason.
#
# THE SPLIT IS ABOUT COST, AND ABOUT WHAT EACH SUITE MEASURES.
#
#   pcg              unit. The compiler's refusals, the dataset's own rules, the override layer, the
#                    derivation key, and provenance. A millisecond each, no world generated.
#   pcg_identity     integration, because the collision sweep is a MEASUREMENT over 65 536 derived
#                    identities rather than an assertion at one of them. Shrinking it to fit a unit
#                    budget would test a different claim than "this derivation does not collide" —
#                    which is exactly the claim the spike found it had to make, having found the
#                    defect in its own.
#   pcg_generation   integration: 64 regions generated whole, three times, then regenerated from an
#                    edit and compared against the full run bit for bit. This is the suite tasks.md
#                    section 0's answer lives in.
#   pcg_scale        integration: a million points through one reserve, measured against an
#                    allocator. "No per-point heap allocation" is a statement about allocations, so
#                    it is counted rather than timed.
#   pcg_adapters     integration, and the ONLY suite that links a representation: cy::pcg-adapters,
#                    cy::foliage and cy::terrain. Everything above it links `cy::pcg` alone, which is
#                    what keeps "a generator does not name the representations it produces into" a
#                    property of the link graph rather than a claim.

set_property(GLOBAL PROPERTY CY_PCG_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_pcg_declare_tests)
    get_property(cy_pcg_tests_dir GLOBAL PROPERTY CY_PCG_TESTS_DIR)

    # cy_add_test() reads the taxonomy from variables that tests/CMakeLists.txt sets in its own
    # directory scope, and this function runs in the top-level scope, which is not a child of it.
    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/pcg/tests: the '${kind}' entry of the test taxonomy could not be read from "
                "tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
                "deferral around it should go away — declare the suites from tests/unit/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME pcg
        KIND unit
        SOURCES
            "${cy_pcg_tests_dir}/test_cache.cpp"
            "${cy_pcg_tests_dir}/test_compile.cpp"
            "${cy_pcg_tests_dir}/test_dataset.cpp"
            "${cy_pcg_tests_dir}/test_overrides.cpp"
            "${cy_pcg_tests_dir}/test_provenance.cpp"
        DEPENDENCIES cy::pcg)

    cy_add_test(
        NAME pcg_identity
        KIND integration
        SOURCES "${cy_pcg_tests_dir}/test_identity.cpp"
        DEPENDENCIES cy::pcg)

    cy_add_test(
        NAME pcg_generation
        KIND integration
        SOURCES
            "${cy_pcg_tests_dir}/test_determinism.cpp"
            "${cy_pcg_tests_dir}/test_invalidation.cpp"
        DEPENDENCIES cy::pcg)

    cy_add_test(
        NAME pcg_scale
        KIND integration
        SOURCES "${cy_pcg_tests_dir}/test_scale.cpp"
        DEPENDENCIES cy::pcg)

    cy_add_test(
        NAME pcg_adapters
        KIND integration
        SOURCES "${cy_pcg_tests_dir}/test_adapters.cpp"
        DEPENDENCIES cy::pcg cy::pcg-adapters)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_pcg_declare_tests)
