# src/foliage/tests/ — CyberFoliage's suites. M10 task 2.4.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred to the end of the top-level directory's processing, and the taxonomy is read
# back out of tests/ rather than restated, because a budget copied into a second file is a budget
# that drifts. src/water/tests/, src/terrain/tests/ and src/environment/tests/ do the same.
#
# THE SPLIT IS ABOUT COST, AND ABOUT WHAT IS BEING MEASURED.
#
#   foliage             unit. Species declarations and their refusals, the compact instance and its
#                       canonical order, the hierarchical cull, wind, the interaction
#                       field, the budget and the promotion state machine. A millisecond each, no
#                       thread and no I/O.
#   foliage_grass       integration, because the ratio the requirement is about only shows at scale:
#                       expanding half a million blades from 240 patch descriptions is 34 ms of CPU,
#                       and a meadow small enough to fit in a millisecond would be a meadow whose
#                       storage ratio proves nothing. The taxonomy's own instruction — "a test this
#                       expensive in the next suite up" — applied rather than argued with.
#   foliage_placement   integration, because it drives a REAL `terrain::TerrainStore` over sixteen
#                       cooked tiles and generates nine regions several times over to compare a
#                       partial regeneration against a full one. That comparison is the M10 spike's
#                       own finding applied to this module, and a version of it small enough to fit
#                       in a millisecond would be a version whose regions never contended.
#   foliage_exceptions  integration: a real `world::PersistenceOverlay`, real regeneration under a
#                       changed rule graph version, and the re-resolution across it.
#   foliage_fields      integration: a real `environment::FieldRegistry` and `FieldStore`, a real
#                       producer claim and its refusal, and the regional state read back out.
#   foliage_streaming   integration: a real `world::CellEventQueue`, real cell identities, clusters
#                       bound and evicted, and the server profile's channel mask.

set_property(GLOBAL PROPERTY CY_FOLIAGE_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_foliage_declare_tests)
    get_property(cy_foliage_tests_dir GLOBAL PROPERTY CY_FOLIAGE_TESTS_DIR)

    # cy_add_test() reads the taxonomy from variables that tests/CMakeLists.txt sets in its own
    # directory scope, and this function runs in the top-level scope, which is not a child of it.
    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/foliage/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
                "the deferral around it should go away — declare the suites from tests/unit/ "
                "instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME foliage
        KIND unit
        SOURCES
            "${cy_foliage_tests_dir}/test_species.cpp"
            "${cy_foliage_tests_dir}/test_instances.cpp"
            "${cy_foliage_tests_dir}/test_wind.cpp"
            "${cy_foliage_tests_dir}/test_interaction.cpp"
            "${cy_foliage_tests_dir}/test_budget.cpp"
            "${cy_foliage_tests_dir}/test_promotion.cpp"
            "${cy_foliage_tests_dir}/test_diagnostics.cpp"
            "${cy_foliage_tests_dir}/test_cook.cpp"
        DEPENDENCIES cy::foliage cy::foliage-render)

    cy_add_test(
        NAME foliage_grass
        KIND integration
        SOURCES "${cy_foliage_tests_dir}/test_grass.cpp"
        DEPENDENCIES cy::foliage)

    cy_add_test(
        NAME foliage_placement
        KIND integration
        SOURCES "${cy_foliage_tests_dir}/test_placement.cpp"
        DEPENDENCIES cy::foliage cy::terrain)

    cy_add_test(
        NAME foliage_exceptions
        KIND integration
        SOURCES "${cy_foliage_tests_dir}/test_exceptions.cpp"
        DEPENDENCIES cy::foliage cy::terrain cy::world)

    cy_add_test(
        NAME foliage_fields
        KIND integration
        SOURCES "${cy_foliage_tests_dir}/test_fields.cpp"
        DEPENDENCIES cy::foliage cy::environment cy::terrain)

    cy_add_test(
        NAME foliage_streaming
        KIND integration
        SOURCES "${cy_foliage_tests_dir}/test_streaming.cpp"
        DEPENDENCIES cy::foliage cy::world)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_foliage_declare_tests)
