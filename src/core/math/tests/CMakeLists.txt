# src/core/math/tests/ — the maths layer's suites. Section 3.1.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred: this file defines one function and asks CMake to call it at the end of the
# top-level directory's processing, by which time the taxonomy has been declared. (A deferred
# add_subdirectory() is rejected outright by CMake — "Subdirectories may not be created during
# deferred execution" — so the deferral has to be of the declaration, not of the directory.)
#
# This is the third module in M1 to hit the same wall and solve it the same way; src/core/reflect/
# and src/core/values/ have the same shape, and whoever closes the milestone should settle it
# properly — either move `add_subdirectory(tests)` above `add_subdirectory(src)` at the top level, or
# lift the taxonomy into a cmake/ module both sides include.
#
# THE UNIT / INTEGRATION SPLIT IS ABOUT COST, NOT ABOUT SUBJECT. A unit test has a millisecond and
# does no I/O. The conventions, the types, the geometry and the curves all fit comfortably. The
# other three do not, and each for a stated reason:
#
#   math_simd     sweeps every pair of two dozen probe values through every primitive on every
#                 compiled backend, and transforms ten thousand points — which is `core-math`'s own
#                 "Bulk transform" scenario and is the whole point of the batch API existing.
#   math_spatial  builds trees over thousands of objects and compares every query against brute
#                 force. A smaller corpus would not catch an omission, and an omission is the only
#                 way an acceleration structure is ever wrong.
#   math_random   measures a distribution. A hundred thousand samples is what makes the mean and
#                 variance assertions meaningful rather than decorative.

# This directory's path travels to the deferred call in a global property. Neither an ordinary
# variable nor a deferred-call argument survives the trip: the call runs in the top-level scope,
# which is not a child of this one, and a global property is the one place both scopes can see.
set_property(GLOBAL PROPERTY CY_MATH_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_math_declare_tests)
    get_property(cy_math_tests_dir GLOBAL PROPERTY CY_MATH_TESTS_DIR)

    # cy_add_test() reads the taxonomy from variables that tests/CMakeLists.txt sets in its own
    # directory scope, and this function runs in the top-level scope, which is not a child of it.
    # They are read back from the directory that defines them rather than restated here: a budget
    # copied into a second file is a budget that drifts.
    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/core/math/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
                "the deferral around it should go away — declare the suites directly from "
                "tests/unit/math/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    # Sources are absolute, because the deferred call runs in the top-level directory's scope and a
    # relative path would be resolved against that directory rather than against this one.
    cy_add_test(
        NAME math
        KIND unit
        SOURCES
            "${cy_math_tests_dir}/test_conventions.cpp"
            "${cy_math_tests_dir}/test_curves.cpp"
            "${cy_math_tests_dir}/test_geometry.cpp"
            "${cy_math_tests_dir}/test_types.cpp"
        DEPENDENCIES cy::core-math)

    cy_add_test(
        NAME math_simd
        KIND integration
        SOURCES "${cy_math_tests_dir}/test_simd.cpp"
        DEPENDENCIES cy::core-math)

    cy_add_test(
        NAME math_spatial
        KIND integration
        SOURCES "${cy_math_tests_dir}/test_spatial.cpp"
        DEPENDENCIES cy::core-math)

    cy_add_test(
        NAME math_random
        KIND integration
        SOURCES "${cy_math_tests_dir}/test_random.cpp"
        DEPENDENCIES cy::core-math)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_math_declare_tests)
