# src/game_backend/lockstep/ — the lockstep session and the adapter behind ABI 1.8's `lockstep_*`.
# openspec/changes/add-deterministic-math stage 8.
#
# A MODULE OF ITS OWN because it links cy::movement, which links cy::core-detmath, which defines
# `CY_DETERMINISM_MATH` for every consumer: linking this adapter is how an embedder says it runs a
# deterministic-math session, and the other adapters, which every host links, must not say so.
cy_add_module(
    NAME cy_game_backend_lockstep
    LAYER abi
    ALIAS cy::game-backend-lockstep
    TYPE STATIC
    SOURCES
        src/lockstep_session.cpp
        src/lockstep_backend.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::abi cy::movement cy::gameplay cy::navigation cy::core-detmath
                        cy::core-determinism)

# Contraction off, as every authoritative module sets it; the session has no float to contract
# after `load()`, and the profile declaration below refuses to configure without the flag.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cy_game_backend_lockstep PRIVATE -ffp-contract=off)
elseif(MSVC)
    target_compile_options(cy_game_backend_lockstep PRIVATE /fp:precise)
endif()

cy_declare_determinism_profile(cy_game_backend_lockstep
    PROFILE Lockstep
    REASON "every per-tick quantity is the mover's, the Fixed crowd's or a Fixed path's; the one float is the baked mesh, converted once in load() by cy::movement before the first tick")

if(CY_BUILD_TESTS)
    set_property(GLOBAL PROPERTY CY_GAME_BACKEND_LOCKSTEP_TESTS_DIR
                 "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    function(cy_game_backend_lockstep_declare_tests)
        get_property(tests_dir GLOBAL PROPERTY CY_GAME_BACKEND_LOCKSTEP_TESTS_DIR)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_integration)
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_integration)
        set(CY_TEST_BUDGET_integration "${budget}")
        set(CY_TEST_TIMEOUT_integration "${timeout}")
        cy_add_test(
            NAME game_backend_lockstep
            KIND integration
            SOURCES "${tests_dir}/test_lockstep_backend.cpp"
            DEPENDENCIES cy::game-backend-lockstep)
        target_include_directories(cy_test_integration_game_backend_lockstep
                                   PRIVATE "${CMAKE_SOURCE_DIR}/src/movement/tests")
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
                   cy_game_backend_lockstep_declare_tests)
endif()
