# src/backends/rhi/tests/ — the RHI's suites. Section 2.1 of the M3 tasks.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred to the end of the top-level directory's processing, by which time the
# taxonomy has been declared; the taxonomy itself is read back out of tests/ rather than restated,
# because a budget copied into a second file is a budget that drifts. src/ecs/, src/scene/,
# src/core/memory/ and src/runtime/ all do the same, for the same reason.
#
#   rhi   the interface's own arithmetic at unit cost: the access table, the hard limits, the format
#         table, and the null backend's handle semantics. Every case builds its own device, because
#         a device carries handle generations and a command log and sharing one would make each case
#         depend on the order the others ran in. A null device is cheap — no driver, no allocation
#         beyond a few pools — and the whole suite measured well inside the millisecond budget.
set_property(GLOBAL PROPERTY CY_RHI_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_rhi_declare_tests)
    get_property(cy_rhi_tests_dir GLOBAL PROPERTY CY_RHI_TESTS_DIR)

    # cy_add_test() reads the taxonomy from variables that tests/CMakeLists.txt sets in its own
    # directory scope, and this function runs in the top-level scope, which is not a child of it.
    foreach(kind IN ITEMS unit)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/backends/rhi/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
                "the deferral around it should go away — declare the suites from tests/unit/ "
                "instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME rhi
        KIND unit
        SOURCES
            "${cy_rhi_tests_dir}/test_access.cpp"
            "${cy_rhi_tests_dir}/test_limits.cpp"
            "${cy_rhi_tests_dir}/test_null_device.cpp"
        DEPENDENCIES cy::rhi cy::rhi-null)

endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_rhi_declare_tests)
