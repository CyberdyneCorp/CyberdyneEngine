# src/servers/text/tests/ — CyberText's suite. M5 task 5.3.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred to the end of the top-level directory's processing, and the taxonomy is read
# back out of tests/ rather than restated. Every module with its own suites does the same.
#
# One suite, and it is unit: nothing here opens a file, starts a thread or touches a device. A font
# is a grid of bytes the test writes, which is exactly the property the module's "a font is data"
# decision was made for.

set_property(GLOBAL PROPERTY CY_TEXT_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_text_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_TEXT_TESTS_DIR)

    foreach(kind IN ITEMS unit)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/servers/text/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
                "the deferral around it should go away — declare the suite from tests/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME text
        KIND unit
        SOURCES
            "${tests_dir}/test_atlas.cpp"
            "${tests_dir}/test_layout.cpp"
            "${tests_dir}/test_server.cpp"
        DEPENDENCIES cy::servers-text)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_text_declare_tests)
