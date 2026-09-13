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
        test_budget_contention.cpp
        test_temp_dir.cpp
        test_wrapper_seam.cpp
    # The seam test reads the test tree it is checking. The path is compiled in rather than
    # discovered, because a test that guesses where the sources are passes vacuously when it guesses
    # wrong.
    DEFINITIONS CY_TESTS_SOURCE_DIR="${cy_tests_dir}")

# The state hash's coverage over a scene world. Here rather than in a module because it is a claim
# about three at once — the ECS's built-in components, the scene's, and the runtime's walk over both
# — and because the thing it defends is a property of the composition: at M2's close the hash was
# silent about thirteen of seventeen subjects, and a suite inside any one module would have been
# able to say only that its own half was declared.
cy_add_test(
    NAME    state_hash_coverage
    KIND    integration
    SOURCES test_state_hash_coverage.cpp
    DEPENDENCIES cy::runtime cy::scene cy::ecs cy::core-determinism cy::core-values)

# The one registry every producing module declares into. Here rather than in any one module because
# the defect it exists for lives BETWEEN modules: `foliage` and `weather` each declared the standard
# `wind` field, differently, and `FieldRegistry::declare()` refuses the second in either order — so
# a project using both rows failed at startup while both modules' own suites stayed green. M10's
# gate found it; this is the check that would have. It links every row that declares a field, which
# is what makes it an integration suite and not a unit one.
cy_add_test(
    NAME    standard_fields
    KIND    integration
    SOURCES test_standard_fields.cpp
    DEPENDENCIES
        cy::environment
        cy::foliage
        cy::rendering-sky
        cy::terrain
        cy::water
        cy::weather)
