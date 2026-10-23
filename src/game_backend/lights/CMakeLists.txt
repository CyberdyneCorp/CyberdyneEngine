# src/game_backend/lights/ — the lights adapter behind ABI 1.11.
#
# A MODULE OF ITS OWN because it links cy::rendering-lighting, which the other adapters do not: a
# host that draws nothing links none of it, binds no `LightsBackend`, and every `light_*` entry
# answers UNAVAILABLE. It is presentation: nothing it holds feeds back into a fixed step.
cy_add_module(
    NAME cy_game_backend_lights
    LAYER abi
    ALIAS cy::game-backend-lights
    TYPE STATIC
    SOURCES src/lights_backend.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::abi cy::rendering-lighting)

if(CY_BUILD_TESTS)
    set_property(GLOBAL PROPERTY CY_GAME_BACKEND_LIGHTS_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    function(cy_game_backend_lights_declare_tests)
        get_property(tests_dir GLOBAL PROPERTY CY_GAME_BACKEND_LIGHTS_TESTS_DIR)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_integration)
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_integration)
        set(CY_TEST_BUDGET_integration "${budget}")
        set(CY_TEST_TIMEOUT_integration "${timeout}")
        cy_add_test(
            NAME game_backend_lights
            KIND integration
            SOURCES "${tests_dir}/test_lights_backend.cpp"
            DEPENDENCIES cy::game-backend-lights)
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_game_backend_lights_declare_tests)
endif()
