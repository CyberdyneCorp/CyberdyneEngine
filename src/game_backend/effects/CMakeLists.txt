# src/game_backend/effects/ — the effects adapter behind ABI 1.15's `effect_*` entries.
#
# A MODULE OF ITS OWN because it links cy::vfx, which `-D CY_VFX=OFF` removes and the other adapters
# do not need: a host that draws nothing links none of it, binds no `EffectsBackend`, and every
# `effect_*` entry answers UNAVAILABLE. It is presentation: nothing it holds feeds back into a fixed
# step.
cy_add_module(
    NAME cy_game_backend_effects
    LAYER abi
    ALIAS cy::game-backend-effects
    TYPE STATIC
    SOURCES src/effects_backend.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::abi cy::vfx)

if(CY_BUILD_TESTS)
    set_property(GLOBAL PROPERTY CY_GAME_BACKEND_EFFECTS_TESTS_DIR
                 "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    function(cy_game_backend_effects_declare_tests)
        get_property(tests_dir GLOBAL PROPERTY CY_GAME_BACKEND_EFFECTS_TESTS_DIR)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_integration)
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_integration)
        set(CY_TEST_BUDGET_integration "${budget}")
        set(CY_TEST_TIMEOUT_integration "${timeout}")
        cy_add_test(
            NAME game_backend_effects
            KIND integration
            SOURCES "${tests_dir}/test_effects_backend.cpp"
                    "${CMAKE_SOURCE_DIR}/src/vfx/tests/effects.cpp"
            DEPENDENCIES cy::game-backend-effects cy::vfx-compiler)
        # The spark plume every VFX suite cooks: compiled here rather than copied, so this suite
        # plays the effect the VFX suites hold to their own claims.
        target_include_directories(cy_test_integration_game_backend_effects
                                   PRIVATE "${CMAKE_SOURCE_DIR}/src/vfx/tests")
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_game_backend_effects_declare_tests)
endif()
