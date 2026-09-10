# src/text/tests/ — the Unicode layer's suite. M8.b task 9.4.
#
# ONE SUITE, AND IT IS UNIT. Nothing here opens a file, starts a thread or touches a device: the
# inputs are string literals and the outputs are levels, break offsets and glyphs.

set_property(GLOBAL PROPERTY CY_TEXT_UNICODE_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_text_unicode_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_TEXT_UNICODE_TESTS_DIR)

    foreach(kind IN ITEMS unit)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR "src/text/tests: the '${kind}' entry of the taxonomy is missing.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME text_unicode
        KIND unit
        SOURCES
            "${tests_dir}/test_bidi.cpp"
            "${tests_dir}/test_linebreak.cpp"
            "${tests_dir}/test_shaping.cpp"
            "${tests_dir}/test_localisation.cpp"
        DEPENDENCIES cy::text-unicode)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_text_unicode_declare_tests)
