# src/scene/serialization/tests/ — the authoring and cooking suites. Section 3.2 of the M2 tasks.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred to the end of the top-level directory's processing, by which time the
# taxonomy has been declared; the taxonomy itself is read back out of tests/ rather than restated,
# because a budget copied into a second file is a budget that drifts. src/ecs/, src/core/values/ and
# src/core/serialize/ do the same, for the same reason.
#
# THE SPLIT IS THE TAXONOMY'S, AND IT IS ABOUT COST.
#
#   scene_serialization        documents, prefabs, overrides, variants, parameters and identity.
#                              Every case is a handful of entities and no I/O.
#   scene_cooking              the cook and the spawn. It builds a `World`, registers components,
#                              allocates chunks and spawns a thousand instances of a template —
#                              which is the number the specification's "spawning a thousand of the
#                              same thing" scenario is stated at, and shrinking it would be testing
#                              a different claim.

set_property(GLOBAL PROPERTY CY_SCENE_SERIALIZATION_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_scene_serialization_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_SCENE_SERIALIZATION_TESTS_DIR)

    # cy_add_test() reads the taxonomy from variables that tests/CMakeLists.txt sets in its own
    # directory scope, and this function runs in the top-level scope, which is not a child of it.
    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/scene/serialization/tests: the '${kind}' entry of the test taxonomy could not "
                "be read from tests/CMakeLists.txt. If the budget variables were renamed, this "
                "lookup and the deferral around it should go away — declare the suites from "
                "tests/unit/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME scene_serialization
        KIND unit
        SOURCES
            "${tests_dir}/test_documents.cpp"
            "${tests_dir}/test_identity.cpp"
            "${tests_dir}/test_prefabs.cpp"
        DEPENDENCIES cy::scene-serialization)

    cy_add_test(
        NAME scene_cooking
        KIND integration
        SOURCES
            "${tests_dir}/test_cook.cpp"
            "${tests_dir}/test_spawn.cpp"
        DEPENDENCIES cy::scene-serialization cy::ecs)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_scene_serialization_declare_tests)
