# src/game_backend/fog/ — the fog of war adapter behind ABI 1.10.
#
# A MODULE OF ITS OWN because it links cy::gameplay-fog, which the other adapters do not: a host
# that runs no fog of war links nothing of it, binds no `FogBackend`, and every `fog_*` entry
# answers UNAVAILABLE. It is simulation — the grid it updates is what each lockstep peer agrees on
# — so it is built with contraction off, although it has no floating point to contract.
cy_add_module(
    NAME cy_game_backend_fog
    LAYER abi
    ALIAS cy::game-backend-fog
    TYPE STATIC
    SOURCES src/fog_backend.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::abi cy::gameplay-fog)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cy_game_backend_fog PRIVATE -ffp-contract=off)
elseif(MSVC)
    target_compile_options(cy_game_backend_fog PRIVATE /fp:precise)
endif()

if(CY_BUILD_TESTS)
    set_property(GLOBAL PROPERTY CY_GAME_BACKEND_FOG_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    function(cy_game_backend_fog_declare_tests)
        get_property(tests_dir GLOBAL PROPERTY CY_GAME_BACKEND_FOG_TESTS_DIR)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_integration)
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_integration)
        set(CY_TEST_BUDGET_integration "${budget}")
        set(CY_TEST_TIMEOUT_integration "${timeout}")
        cy_add_test(
            NAME game_backend_fog
            KIND integration
            SOURCES "${tests_dir}/test_fog_backend.cpp"
            DEPENDENCIES cy::game-backend-fog)
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_game_backend_fog_declare_tests)
endif()
