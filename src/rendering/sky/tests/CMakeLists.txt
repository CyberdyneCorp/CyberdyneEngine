# src/rendering/sky/tests/ — the sky suite. M7 task 10.4.
#
#   render_sky        unit. The atmosphere's own invariants and the celestial model: what an
#                     atmosphere may not be, that the phase functions are normalised, that the sun
#                     reddens because its path lengthened, that a different planet gets a different
#                     sky from its coefficients alone, and that the clock is a declared domain.
#                     None of these integrates the sky more than a handful of times.
#   render_sky_light  integration, and it is the CRITERION of task 10.4. "An analytic sky sufficient
#                     for GI's sky term" is a claim about an INTEGRAL — the irradiance the
#                     three-colour gradient delivers against the irradiance the full atmosphere
#                     delivers, over a day and over several surface orientations — and each of those
#                     integrals is two thousand ray marches. A version cheap enough for a 1 ms unit
#                     budget would be a spot check, and a gradient that agreed with the sky only at
#                     the zenith would pass one.
set_property(GLOBAL PROPERTY CY_RENDER_SKY_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_render_sky_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_RENDER_SKY_TESTS_DIR)

    foreach(kind unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/sky/tests: the '${kind}' entry of the test taxonomy could not be "
                "read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME render_sky
        KIND unit
        SOURCES "${tests_dir}/test_sky.cpp"
        DEPENDENCIES cy::rendering-sky)

    cy_add_test(
        NAME render_sky_light
        KIND integration
        SOURCES "${tests_dir}/test_sky_light.cpp"
        DEPENDENCIES cy::rendering-sky)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_render_sky_declare_tests)
