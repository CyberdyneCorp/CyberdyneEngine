# src/backends/rhi-metal/tests/ — the Metal seed's suite. M7 task 10.5.
#
#   rhi_metal_seed  unit, and it runs ON A MACHINE WITH NO METAL. That is the whole shape of this
#                   module: the findings are data, so they are testable wherever the engine builds,
#                   and a finding nobody can read on the machine they are working on is a finding
#                   nobody reads. `src/device.mm` is not covered by anything and has never been
#                   compiled — see this module's README.md, which says so in its first paragraph.
set_property(GLOBAL PROPERTY CY_RHI_METAL_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_rhi_metal_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_RHI_METAL_TESTS_DIR)

    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_unit)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_unit)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/backends/rhi-metal/tests: the 'unit' entry of the test taxonomy could not be read "
            "from tests/CMakeLists.txt.")
    endif()
    set(CY_TEST_BUDGET_unit "${budget}")
    set(CY_TEST_TIMEOUT_unit "${timeout}")

    cy_add_test(
        NAME rhi_metal_seed
        KIND unit
        SOURCES "${tests_dir}/test_metal_seed.cpp"
        DEPENDENCIES cy::rhi-metal)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_rhi_metal_declare_tests)
