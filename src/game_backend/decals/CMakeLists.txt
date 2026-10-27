# src/game_backend/decals/ — the decals adapter behind ABI 1.15's `decal_*` entries.
#
# A MODULE OF ITS OWN because it links cy::rendering-lighting, which the core adapters do not: a host
# that draws nothing links none of it, binds no `DecalsBackend`, and every `decal_*` entry answers
# UNAVAILABLE. It is presentation: nothing it holds feeds back into a fixed step.
cy_add_module(
    NAME cy_game_backend_decals
    LAYER abi
    ALIAS cy::game-backend-decals
    TYPE STATIC
    SOURCES src/decals_backend.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::abi cy::rendering-lighting)

if(CY_BUILD_TESTS)
    set_property(GLOBAL PROPERTY CY_GAME_BACKEND_DECALS_TESTS_DIR
                 "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    function(cy_game_backend_decals_declare_tests)
        get_property(tests_dir GLOBAL PROPERTY CY_GAME_BACKEND_DECALS_TESTS_DIR)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_integration)
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_integration)
        set(CY_TEST_BUDGET_integration "${budget}")
        set(CY_TEST_TIMEOUT_integration "${timeout}")
        cy_add_test(
            NAME game_backend_decals
            KIND integration
            SOURCES "${tests_dir}/test_decals_backend.cpp"
            DEPENDENCIES cy::game-backend-decals)
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_game_backend_decals_declare_tests)
endif()
