# tests/integration/ — subsystems together, and anything that touches the world outside the process.
#
# A second per test case. Filesystem, several modules at once, a server with a null backend: work
# that is real enough to be worth a second and cheap enough to run before every push.
#
# The suites here are flat rather than one directory per module, because an integration test is
# about a seam between modules and has no single owner to file it under.

# The directory the seam test scans: tests/ itself, resolved once rather than spelled with a ".."
# that every reader has to re-resolve.
get_filename_component(cy_tests_dir "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)

cy_add_test(
    NAME    harness
    KIND    integration
    SOURCES
        test_temp_dir.cpp
        test_wrapper_seam.cpp
    # The seam test reads the test tree it is checking. The path is compiled in rather than
    # discovered, because a test that guesses where the sources are passes vacuously when it guesses
    # wrong.
    DEFINITIONS CY_TESTS_SOURCE_DIR="${cy_tests_dir}")
