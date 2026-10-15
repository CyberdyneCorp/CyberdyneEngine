# src/game_backend/illumination/ — the GI adapter behind ABI 1.9. Issue #35.
#
# A MODULE OF ITS OWN because it names `cy::rendering-gi`, a rendering module the other adapters do
# not link: a host that runs no GI links nothing of it, binds no `IlluminationBackend`, and every
# `gi_*` entry answers UNAVAILABLE.
cy_add_module(
    NAME cy_game_backend_illumination
    LAYER abi
    ALIAS cy::game-backend-illumination
    TYPE STATIC
    SOURCES src/illumination_backend.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::abi cy::rendering-gi)

if(CY_BUILD_TESTS)
    set_property(GLOBAL PROPERTY CY_GAME_BACKEND_ILLUMINATION_TESTS_DIR
                 "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    function(cy_game_backend_illumination_declare_tests)
        get_property(tests_dir GLOBAL PROPERTY CY_GAME_BACKEND_ILLUMINATION_TESTS_DIR)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_integration)
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_integration)
        set(CY_TEST_BUDGET_integration "${budget}")
        set(CY_TEST_TIMEOUT_integration "${timeout}")
        cy_add_test(
            NAME game_backend_illumination
            KIND integration
            SOURCES "${tests_dir}/test_illumination_backend.cpp"
            DEPENDENCIES cy::game-backend-illumination cy::ecs)
        target_include_directories(cy_test_integration_game_backend_illumination
                                   PRIVATE "${CMAKE_SOURCE_DIR}/src/rendering/gi/tests")
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
                   cy_game_backend_illumination_declare_tests)
endif()
