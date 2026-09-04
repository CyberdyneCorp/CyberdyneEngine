# src/core/serialize/tests/ — the serialization module's suites. Section 3.2 of the M2 tasks.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred to the end of the top-level directory's processing, by which time the
# taxonomy has been declared; the taxonomy itself is read back out of tests/ rather than restated,
# because a budget copied into a second file is a budget that drifts. src/core/values/, src/ecs/ and
# every other module with its own suites do the same, for the same reason.
#
# TWO SUITES, AND THE SPLIT IS THE TAXONOMY'S, ABOUT COST RATHER THAN SUBJECT.
#
#   serialize        the model: the wire, value records, the tagged form, the text form, migration
#                    and the cooked container. Every case is a few records and a few hundred bytes.
#   serialize_scale  the one case whose number is the claim: four thousand rows of packed component
#                    data, to show the cooked form's per-row overhead is zero. Measured at 1.5-1.8 ms
#                    in the Debug configuration, which is over the unit budget — so it moved here
#                    rather than shrinking, because the row count is what is being asserted.

set_property(GLOBAL PROPERTY CY_CORE_SERIALIZE_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_core_serialize_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_CORE_SERIALIZE_TESTS_DIR)

    # cy_add_test() reads the taxonomy from variables that tests/CMakeLists.txt sets in its own
    # directory scope, and this function runs in the top-level scope, which is not a child of it.
    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/core/serialize/tests: the '${kind}' entry of the test taxonomy could not be "
                "read from tests/CMakeLists.txt. If the budget variables were renamed, this lookup "
                "and the deferral around it should go away — declare the suites from tests/unit/ "
                "instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME serialize
        KIND unit
        SOURCES
            "${tests_dir}/test_cooked.cpp"
            "${tests_dir}/test_migration.cpp"
            "${tests_dir}/test_tagged.cpp"
            "${tests_dir}/test_text.cpp"
            "${tests_dir}/test_value_record.cpp"
            "${tests_dir}/test_wire.cpp"
        DEPENDENCIES cy::core-serialize)

    cy_add_test(
        NAME serialize_scale
        KIND integration
        SOURCES "${tests_dir}/test_scale.cpp"
        DEPENDENCIES cy::core-serialize)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_core_serialize_declare_tests)
