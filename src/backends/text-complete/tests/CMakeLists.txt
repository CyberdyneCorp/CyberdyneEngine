# src/backends/text-complete/tests/ — `integration.text_complete`: the complete text backend over
# the fonts in deps/fonts/. M11.e, issue #86.
#
# `integration`, not `unit`, for two reasons the taxonomy names: every case reads a font from disk,
# which is I/O, and the distance-field and variable-font cases rasterise at several sizes, which is
# milliseconds rather than microseconds. The server's own logic — face runs, bidirectional line
# layout, cooked fonts, the per-frame report — is tested without any of this in `unit.text`, over a
# fake backend, so it runs in a build with CY_TEXT off.
#
# THE TARGET LINKS `cy::text-complete` AND NOT A LIBRARY. A case that named an FT_, hb_ or U type
# would not compile, because the libraries' include directories are PRIVATE to the backend.
set_property(GLOBAL PROPERTY CY_TEXT_COMPLETE_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_text_complete_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_TEXT_COMPLETE_TESTS_DIR)
    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_integration)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_integration)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/backends/text-complete/tests: the 'integration' entry of the test taxonomy could "
            "not be read from tests/CMakeLists.txt.")
    endif()
    set(CY_TEST_BUDGET_integration "${budget}")
    set(CY_TEST_TIMEOUT_integration "${timeout}")

    cy_add_test(
        NAME text_complete
        KIND integration
        SOURCES
            "${tests_dir}/test_complete.cpp"
            "${tests_dir}/test_shaping.cpp"
        DEPENDENCIES cy::text-complete)
    target_compile_definitions(cy_test_integration_text_complete PRIVATE
        CY_TEXT_FONTS_DIR="${CMAKE_SOURCE_DIR}/deps/fonts")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_text_complete_declare_tests)
