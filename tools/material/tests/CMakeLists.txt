# tools/material/tests/ — the material cook's suite. M7 task 6.3.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* tools/, so that function does not exist while this file is read. The
# declaration is therefore deferred to the end of the top-level directory's processing, and the
# taxonomy is read back out of tests/ rather than restated. tools/build/tests/, tools/cook/tests/
# and tools/import/tests/ all do the same.
#
# INTEGRATION, because every case writes real files and runs a real build service: what is being
# checked is that the cook is a NODE — that its key covers the toolchain and the compiler version,
# that a second run is a cache hit, that two runs produce identical bytes, and that a material with
# a cook-time error produces no artefact at all. None of that is observable without a store.
set_property(GLOBAL PROPERTY CY_MATERIAL_COOK_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_material_cook_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_MATERIAL_COOK_TESTS_DIR)

    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_integration)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_integration)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "tools/material/tests: the 'integration' entry of the test taxonomy could not be read "
            "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
            "deferral around it should go away — declare the suite from tests/integration/ instead.")
    endif()
    set(CY_TEST_BUDGET_integration "${budget}")
    set(CY_TEST_TIMEOUT_integration "${timeout}")

    cy_add_test(
        NAME material_cook
        KIND integration
        SOURCES "${tests_dir}/test_material_cook.cpp"
        DEPENDENCIES cy::material-cook cy::build-graph)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_material_cook_declare_tests)
