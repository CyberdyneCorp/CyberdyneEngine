# src/weather/tests/ — CyberWeather's suites. M10 tasks 3.1 and 3.2.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred to the end of the top-level directory's processing, and the taxonomy is read
# back out of tests/ rather than restated, because a budget copied into a second file is a budget
# that drifts. src/water/tests/ and src/environment/tests/ do the same, for the same reason.
#
# THE SPLIT IS ABOUT COST, AND ABOUT WHAT IS BEING MEASURED.
#
#   weather              unit. The climate map and its biome potential, the cell hierarchy and its
#                        rain shadow, the wind composition and its budget, the environment sample's
#                        quality tiers, the precipitation plan and the occlusion raster, storms and
#                        their wire form, presets and their per-property clocks, and the inspector.
#                        A millisecond each; no store, no thread, no I/O.
#   weather_fields       integration, because it drives a real `environment::FieldRegistry` and
#                        `FieldStore`, claims real producer tokens and publishes real tiles — the
#                        only way "weather publishes state and touches nothing" is measured rather
#                        than asserted, and the only way the SECOND PRODUCER REFUSAL can be run at
#                        all.
#   weather_ecosystem    integration: ninety simulated days of macro evolution against the
#                        substrate's own recovery, compared step for step against ninety one-day
#                        advances. It is the taxonomy's own instruction — "a test this expensive in
#                        the next suite up" — applied rather than argued with.
#   weather_determinism  integration: two systems seeded identically and ticked in lockstep, one at
#                        the lowest presentation levers and one at the highest, with their
#                        authoritative state compared bit for bit. It needs both systems live for a
#                        thousand ticks, which is not a unit test's millisecond.

set_property(GLOBAL PROPERTY CY_WEATHER_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_weather_declare_tests)
    get_property(cy_weather_tests_dir GLOBAL PROPERTY CY_WEATHER_TESTS_DIR)

    # cy_add_test() reads the taxonomy from variables that tests/CMakeLists.txt sets in its own
    # directory scope, and this function runs in the top-level scope, which is not a child of it.
    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/weather/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
                "the deferral around it should go away — declare the suites from tests/unit/ "
                "instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME weather
        KIND unit
        SOURCES
            "${cy_weather_tests_dir}/test_climate.cpp"
            "${cy_weather_tests_dir}/test_cells.cpp"
            "${cy_weather_tests_dir}/test_wind.cpp"
            "${cy_weather_tests_dir}/test_sampling.cpp"
            "${cy_weather_tests_dir}/test_precipitation.cpp"
            "${cy_weather_tests_dir}/test_storms.cpp"
            "${cy_weather_tests_dir}/test_presets.cpp"
            "${cy_weather_tests_dir}/test_diagnostics.cpp"
        DEPENDENCIES cy::weather)

    cy_add_test(
        NAME weather_fields
        KIND integration
        SOURCES "${cy_weather_tests_dir}/test_fields.cpp"
        DEPENDENCIES cy::weather cy::environment)

    cy_add_test(
        NAME weather_ecosystem
        KIND integration
        SOURCES "${cy_weather_tests_dir}/test_ecosystem.cpp"
        DEPENDENCIES cy::weather cy::environment)

    cy_add_test(
        NAME weather_determinism
        KIND integration
        SOURCES "${cy_weather_tests_dir}/test_determinism.cpp"
        DEPENDENCIES cy::weather cy::environment)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_weather_declare_tests)
