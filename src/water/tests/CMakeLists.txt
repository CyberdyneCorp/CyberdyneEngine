# src/water/tests/ — CyberWater's suites. M10 task 2.3.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred to the end of the top-level directory's processing, and the taxonomy is read
# back out of tests/ rather than restated, because a budget copied into a second file is a budget
# that drifts. src/environment/tests/ and src/world/tests/ do the same, for the same reason.
#
# THE SPLIT IS ABOUT COST, AND HERE IT IS ALSO ABOUT WHAT IS BEING MEASURED.
#
#   water            unit. The body registry and its refusals, the displacement contract, the
#                    spectrum and the camera-relative patch, river networks and their flow, the
#                    query, buoyancy, navigation costs, foam and the shading parameters. A
#                    millisecond each, no thread and no I/O.
#   water_foam       integration, because persistence is measured over TIME: a wake is deposited,
#                    advected across a grid for a couple of hundred steps and watched to decay, and
#                    a field small enough to do that inside a millisecond would be a field whose
#                    drift is one cell. The taxonomy's own instruction — "a test this expensive in
#                    the next suite up" — applied rather than argued with.
#   water_shoreline  integration, because it drives a real `environment::FieldRegistry` and
#                    `FieldStore`, claims real producer tokens and publishes real tiles over
#                    thousands of lattice points — the only way "terrain reads fields and water
#                    reads terrain through one callback" is measured rather than asserted.
#   water_streaming  integration: a real `world::CellEventQueue`, real cell identities and real
#                    footprints, cells bound and evicted, and the server profile's payload mask
#                    compared against the client's.

set_property(GLOBAL PROPERTY CY_WATER_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_water_declare_tests)
    get_property(cy_water_tests_dir GLOBAL PROPERTY CY_WATER_TESTS_DIR)

    # cy_add_test() reads the taxonomy from variables that tests/CMakeLists.txt sets in its own
    # directory scope, and this function runs in the top-level scope, which is not a child of it.
    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/water/tests: the '${kind}' entry of the test taxonomy could not be read from "
                "tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
                "deferral around it should go away — declare the suites from tests/unit/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME water
        KIND unit
        SOURCES
            "${cy_water_tests_dir}/test_bodies.cpp"
            "${cy_water_tests_dir}/test_displacement.cpp"
            "${cy_water_tests_dir}/test_ocean.cpp"
            "${cy_water_tests_dir}/test_queries.cpp"
            "${cy_water_tests_dir}/test_rivers.cpp"
            "${cy_water_tests_dir}/test_shading.cpp"
        DEPENDENCIES cy::water)

    cy_add_test(
        NAME water_foam
        KIND integration
        SOURCES "${cy_water_tests_dir}/test_foam.cpp"
        DEPENDENCIES cy::water)

    cy_add_test(
        NAME water_shoreline
        KIND integration
        SOURCES "${cy_water_tests_dir}/test_shoreline.cpp"
        DEPENDENCIES cy::water cy::environment)

    cy_add_test(
        NAME water_streaming
        KIND integration
        SOURCES "${cy_water_tests_dir}/test_streaming.cpp"
        DEPENDENCIES cy::water cy::world)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_water_declare_tests)
