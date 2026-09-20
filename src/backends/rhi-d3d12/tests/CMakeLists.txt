set_property(GLOBAL PROPERTY CY_RHI_D3D12_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_rhi_d3d12_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_RHI_D3D12_TESTS_DIR)
    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_unit)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_unit)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/backends/rhi-d3d12/tests: the test taxonomy could not be read from "
            "tests/CMakeLists.txt.")
    endif()
    set(CY_TEST_BUDGET_unit "${budget}")
    set(CY_TEST_TIMEOUT_unit "${timeout}")

    cy_add_test(
        NAME rhi_d3d12
        KIND unit
        SOURCES "${tests_dir}/test_d3d12_device.cpp"
        DEPENDENCIES cy::rhi-d3d12 cy::rhi-null)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_rhi_d3d12_declare_tests)
