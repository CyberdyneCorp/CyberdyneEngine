# The harness testing itself. Until src/core/ lands there is nothing else at this layer to test, and
# a harness whose own assertions are unverified is not a foundation — a wrapper that quietly turned
# CY_REQUIRE into a no-op would make every suite above it green.

cy_add_test(
    NAME    harness
    KIND    unit
    SOURCES
        test_assertions.cpp
        test_budget.cpp
        test_fixtures.cpp)
