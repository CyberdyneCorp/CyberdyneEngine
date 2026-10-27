# src/animation/ground/tests/ — the two grounds foot placement is handed. Issue #92.
#
#   animation_ground   integration. A reference physics world and a terrain answering downward
#                      probes, the character's own body skipped, and foot placement standing a pair
#                      of legs on a physics step. A physics world is created per case.
#
# Declared after the top-level directory's processing, as src/animation/tests/ does and for the
# same reason: cy_add_test() is defined by tests/, which is added after src/.
set_property(GLOBAL PROPERTY CY_ANIMATION_GROUND_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_animation_ground_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_ANIMATION_GROUND_TESTS_DIR)
    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_integration)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_integration)
    set(CY_TEST_BUDGET_integration "${budget}")
    set(CY_TEST_TIMEOUT_integration "${timeout}")
    cy_add_test(
        NAME animation_ground
        KIND integration
        SOURCES "${tests_dir}/test_ground_probes.cpp"
        DEPENDENCIES cy::animation-ground cy::physics-reference)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_animation_ground_declare_tests)
