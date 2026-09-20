set_property(GLOBAL PROPERTY CY_RHI_D3D12_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_rhi_d3d12_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_RHI_D3D12_TESTS_DIR)
    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_unit)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_unit)
    get_directory_property(integration_budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_integration)
    get_directory_property(integration_timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_integration)
    if(NOT budget OR NOT timeout OR NOT integration_budget OR NOT integration_timeout)
        message(FATAL_ERROR
            "src/backends/rhi-d3d12/tests: the test taxonomy could not be read from "
            "tests/CMakeLists.txt.")
    endif()
    set(CY_TEST_BUDGET_unit "${budget}")
    set(CY_TEST_TIMEOUT_unit "${timeout}")
    set(CY_TEST_BUDGET_integration "${integration_budget}")
    set(CY_TEST_TIMEOUT_integration "${integration_timeout}")

    cy_add_test(
        NAME rhi_d3d12
        KIND unit
        SOURCES "${tests_dir}/test_d3d12_memory_policy.cpp"
        DEPENDENCIES cy::rhi-d3d12)

    cy_add_test(
        NAME rhi_d3d12
        KIND integration
        SOURCES "${tests_dir}/test_d3d12_device.cpp"
        DEPENDENCIES cy::rhi-d3d12 cy::rhi-null)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_rhi_d3d12_declare_tests)
