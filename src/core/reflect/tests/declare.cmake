# The reflection and identity tests. Sections 1.1 and 1.2.
#
# Declared through a deferred function call rather than through add_subdirectory(), because
# cy_add_test() is declared by tests/CMakeLists.txt and the top-level adds src/ before tests/. A
# module cannot add a subdirectory during deferred execution, so this file defines the declarations
# and the parent defers the call — which runs once the whole tree has been processed and
# cy_add_test() exists. Declaring these targets with cy_add_module() instead would mean a second
# copy of the taxonomy's budgets and timeouts, which is the duplication cy_add_test() exists to
# prevent.
#
# The directory is a cache variable because a deferred call runs in the top-level scope, where a
# plain variable set here is not visible.

set(CY_REFLECT_TESTS_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL
    "Where src/core/reflect/'s tests live, for the deferred declaration")

function(cy_reflect_declare_tests)
    # The taxonomy's budgets and timeouts are set in tests/CMakeLists.txt, whose directory scope this
    # deferred call is not in. They are read from there rather than copied: `testing-and-quality`
    # fixes those numbers, tests/CMakeLists.txt is where they live, and a second copy would drift.
    foreach(kind IN ITEMS unit integration smoke)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "cy_reflect_declare_tests: tests/CMakeLists.txt no longer defines "
                "CY_TEST_BUDGET_${kind} and CY_TEST_TIMEOUT_${kind}. They are the taxonomy; find "
                "where they moved rather than restating them here.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(NAME reflect_registry KIND unit
        SOURCES "${CY_REFLECT_TESTS_DIR}/test_registry.cpp"
        DEPENDENCIES cy::core-reflect)

    cy_add_test(NAME reflect_attributes KIND unit
        SOURCES "${CY_REFLECT_TESTS_DIR}/test_attributes.cpp"
        DEPENDENCIES cy::core-reflect)

    cy_add_test(NAME reflect_control_plane KIND unit
        SOURCES "${CY_REFLECT_TESTS_DIR}/test_control_plane.cpp"
        DEPENDENCIES cy::core-reflect)

    # The goldens are committed inputs, so the test is told where they are rather than being run
    # from a working directory that happens to contain them.
    cy_add_test(NAME reflect_roundtrip KIND unit
        SOURCES "${CY_REFLECT_TESTS_DIR}/test_roundtrip.cpp"
        DEPENDENCIES cy::core-reflect
        DEFINITIONS CY_REFLECT_GOLDEN_DIR="${CY_REFLECT_TESTS_DIR}/golden")

    # The generator, the manifest and the identity gate, exercised as programs. The gate is proved
    # by renaming a field in a real tree and watching generation fail, which no C++ test can do to
    # itself.
    add_test(NAME integration.reflect_generator
        COMMAND "${CY_REFLECT_PYTHON}" "${CY_REFLECT_TESTS_DIR}/test_generator.py"
                --source-root "${CMAKE_SOURCE_DIR}")
    set_tests_properties(integration.reflect_generator PROPERTIES
        TIMEOUT 600 LABELS integration)
endfunction()
