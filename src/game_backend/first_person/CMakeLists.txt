# src/game_backend/first_person/ — the first-person adapter behind ABI 1.13.
#
# A MODULE OF ITS OWN because it links cy::rendering-viewmodel, which the other adapters do not: a
# host that draws nothing links none of it, binds no `FirstPersonBackend`, and every
# `first_person_*` and `viewmodel_*` entry answers UNAVAILABLE. It is presentation: nothing it holds
# feeds back into a fixed step.
cy_add_module(
    NAME cy_game_backend_first_person
    LAYER abi
    ALIAS cy::game-backend-first-person
    TYPE STATIC
    SOURCES src/first_person_backend.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::abi cy::servers-camera cy::rendering-viewmodel)

if(CY_BUILD_TESTS)
    set_property(GLOBAL PROPERTY CY_GAME_BACKEND_FIRST_PERSON_TESTS_DIR
                 "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    function(cy_game_backend_first_person_declare_tests)
        get_property(tests_dir GLOBAL PROPERTY CY_GAME_BACKEND_FIRST_PERSON_TESTS_DIR)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_integration)
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_integration)
        set(CY_TEST_BUDGET_integration "${budget}")
        set(CY_TEST_TIMEOUT_integration "${timeout}")
        cy_add_test(
            NAME game_backend_first_person
            KIND integration
            SOURCES "${tests_dir}/test_first_person_backend.cpp"
            DEPENDENCIES cy::game-backend-first-person)
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
                   CALL cy_game_backend_first_person_declare_tests)
endif()
