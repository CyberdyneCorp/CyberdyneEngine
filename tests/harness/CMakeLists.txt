# tests/harness/ — the framework seam and the fixtures, linked by every test binary.
#
# This is the one place doctest is named. cy::dep::doctest carries
# DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS (cmake/dependencies.cmake), which is what makes
# the framework usable under -fno-exceptions, and it is a PUBLIC dependency here because
# cy/test/test.h includes doctest's header: a test binary compiles against it.

cy_add_module(
    NAME  cy_test_harness
    LAYER tools
    ALIAS cy::test-harness
    SOURCES
        src/budget.cpp
        src/fixtures.cpp
        src/main.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::dep::doctest)
