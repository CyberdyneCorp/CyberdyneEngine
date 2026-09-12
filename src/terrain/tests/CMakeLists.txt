# src/terrain/tests/ — CyberTerrain's suites. M10 section 2.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred to the end of the top-level directory's processing, and the taxonomy is read
# back out of tests/ rather than restated, because a budget copied into a second file is a budget
# that drifts. src/world/tests/ and src/environment/tests/ do the same, for the same reason.
#
# THE SPLIT IS ABOUT COST, AND HERE IT IS ALSO ABOUT WHAT IS BEING MEASURED.
#
#   terrain            unit. Tile identity and the replaceable scheme, the sparse hierarchy, the
#                      query interface and its column, material layers and their bound, the rules
#                      and the painting, the page producer's invalidation, the deformation classes
#                      and the delta's overlay round trip, and the modifier stack. Milliseconds
#                      each, no thread and no I/O.
#   terrain_authoring  integration: the modifier stack. One evaluation of one tile is a padded
#                      77x77 lattice through four octaves of value noise — a hundred thousand
#                      stream draws — and several cases evaluate a tile twice to compare. That is
#                      over the unit budget by design; making it cheaper would mean testing a
#                      generator too small to have a halo worth checking.
#   terrain_persistence integration: the overlay round trip and the replication message. Both deform
#                      a world several times over — a crater is a stamp over every sample of every
#                      tile it touches — which is over the unit budget by design.
#   terrain_geometry   integration: meshing a 65x65 lattice, the level stitch measured against the
#                      COARSE tile's own samples, determinism over two builds of the same tile, and
#                      the collision and navigation surfaces derived from it. Tens of thousands of
#                      samples, which is over the unit budget by design.
#   terrain_cook       integration: a tile through `rendering::vg::build_geometry()` and the terrain
#                      surface material through `compile_material()`. It links the cook target, so
#                      it is separate for the reason the target is.

set_property(GLOBAL PROPERTY CY_TERRAIN_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_terrain_declare_tests)
    get_property(cy_terrain_tests_dir GLOBAL PROPERTY CY_TERRAIN_TESTS_DIR)

    # cy_add_test() reads the taxonomy from variables that tests/CMakeLists.txt sets in its own
    # directory scope, and this function runs in the top-level scope, which is not a child of it.
    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/terrain/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
                "the deferral around it should go away — declare the suites from tests/unit/ "
                "instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME terrain
        KIND unit
        SOURCES
            "${cy_terrain_tests_dir}/test_deformation.cpp"
            "${cy_terrain_tests_dir}/test_material.cpp"
            "${cy_terrain_tests_dir}/test_producer.cpp"
            "${cy_terrain_tests_dir}/test_surface.cpp"
            "${cy_terrain_tests_dir}/test_tiles.cpp"
        DEPENDENCIES cy::terrain cy::environment cy::world)

    cy_add_test(
        NAME terrain_authoring
        KIND integration
        SOURCES "${cy_terrain_tests_dir}/test_stack.cpp"
        DEPENDENCIES cy::terrain)

    cy_add_test(
        NAME terrain_persistence
        KIND integration
        SOURCES "${cy_terrain_tests_dir}/test_persistence.cpp"
        DEPENDENCIES cy::terrain cy::world)

    cy_add_test(
        NAME terrain_geometry
        KIND integration
        SOURCES "${cy_terrain_tests_dir}/test_geometry.cpp"
        DEPENDENCIES cy::terrain cy::navigation)

    cy_add_test(
        NAME terrain_cook
        KIND integration
        SOURCES "${cy_terrain_tests_dir}/test_cook.cpp"
        DEPENDENCIES cy::terrain-cook cy::terrain)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_terrain_declare_tests)
