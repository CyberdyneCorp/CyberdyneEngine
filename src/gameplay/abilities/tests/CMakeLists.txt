# src/gameplay/abilities/tests/ — CyberAbilities' suites. M8.b section 4.
#
#   ability_core    unit. Attributes and the modifier order, effects and stacking, sets, grants,
#                   cooldowns and the transactional cost ledger, and the activation pipeline
#                   including a graph compiled by `cy::graph::script::compile_ability`. No world,
#                   no renderer, no audio device and no GPU: `gameplay-framework`'s "Headless
#                   operation" is inherited by every module built on it.
#
#   ability_scale   integration. THE MILESTONE'S EXIT CRITERION FOR THIS MODULE — a hundred
#                   concurrent gameplay effects against a declared budget — plus the linearity case
#                   that is what actually catches a quadratic, and ten thousand activations over one
#                   shared ability. `integration` and not `unit` because each case builds a store, a
#                   hundred to ten thousand owners and a live effect set and then runs a thousand
#                   ticks; the unit tier's budget is a millisecond of CPU and a scale case that fits
#                   in it is a scale case whose population was trimmed until it measured nothing.
#                   That is the remedy M8.a's closing gate applied to batch spawning.
set_property(GLOBAL PROPERTY CY_GAMEPLAY_ABILITIES_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_gameplay_abilities_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_GAMEPLAY_ABILITIES_TESTS_DIR)

    foreach(kind unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/gameplay/abilities/tests: the '${kind}' entry of the test taxonomy could not "
                "be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME ability_core
        KIND unit
        SOURCES
            "${tests_dir}/test_abilities.cpp"
            "${tests_dir}/test_activation.cpp"
            "${tests_dir}/test_attributes.cpp"
            "${tests_dir}/test_effects.cpp"
        DEPENDENCIES cy::gameplay-abilities)

    cy_add_test(
        NAME ability_scale
        KIND integration
        SOURCES "${tests_dir}/test_scale.cpp"
        DEPENDENCIES cy::gameplay-abilities)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_gameplay_abilities_declare_tests)
