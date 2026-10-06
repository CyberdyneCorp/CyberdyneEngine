# src/game_backend/animation/ — the animation adapter behind ABI 1.7. Issue #76 stage 4.
#
# A MODULE OF ITS OWN, AND ONLY WITH `CY_ANIMATION`. The other adapters are compiled into
# cy::game-backend in every build; this one names `cy::animation-system`, which `-D CY_ANIMATION=OFF`
# removes, so it is declared beside them only when the runtime exists — the arrangement
# src/physics/ragdoll/ has with the same option. Without it the `animation_*` entries answer
# UNAVAILABLE, as every entry does whose backend a host did not bind.
cy_add_module(
    NAME cy_game_backend_animation
    LAYER abi
    ALIAS cy::game-backend-animation
    TYPE STATIC
    SOURCES src/animation_backend.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::abi cy::animation cy::animation-system cy::ecs cy::scene)

if(CY_BUILD_TESTS)
    set_property(GLOBAL PROPERTY CY_GAME_BACKEND_ANIMATION_TESTS_DIR
                 "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    function(cy_game_backend_animation_declare_tests)
        get_property(tests_dir GLOBAL PROPERTY CY_GAME_BACKEND_ANIMATION_TESTS_DIR)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_integration)
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_integration)
        set(CY_TEST_BUDGET_integration "${budget}")
        set(CY_TEST_TIMEOUT_integration "${timeout}")
        cy_add_test(
            NAME game_backend_animation
            KIND integration
            SOURCES "${tests_dir}/test_animation_backend.cpp"
            DEPENDENCIES cy::game-backend cy::game-backend-animation cy::graph
                         cy::physics-reference cy::runtime)
        target_include_directories(cy_test_integration_game_backend_animation
                                   PRIVATE "${CMAKE_SOURCE_DIR}/src/animation/tests"
                                           "${CMAKE_SOURCE_DIR}/src/game_backend/tests")
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
                   cy_game_backend_animation_declare_tests)
endif()
