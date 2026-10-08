# The harness testing itself. Until src/core/ lands there is nothing else at this layer to test, and
# a harness whose own assertions are unverified is not a foundation — a wrapper that quietly turned
# CY_REQUIRE into a no-op would make every suite above it green.

cy_add_test(
    NAME    harness
    KIND    unit
    SOURCES
        test_assertions.cpp
        test_budget.cpp
        test_fixtures.cpp
        test_windows_macros.cpp)

# ITS OWN BINARY, AND THE EXECUTABLE'S CHECK IS ITS FIRST CASE. The check is that every page of the
# executable is mapped before the first case runs, so it has to BE the first case: in the suite
# above, the cases sorted before it fault most of the image in themselves and it passes with the
# warm-up removed. The second case checks the C math library the same way; nothing before it calls
# into libm, so it fails with the libm warm-up removed.
cy_add_test(
    NAME    harness_image_warmup
    KIND    unit
    SOURCES test_image_warmup.cpp)
