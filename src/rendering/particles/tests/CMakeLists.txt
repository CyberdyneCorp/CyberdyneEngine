# src/rendering/particles/tests/ — the particle module's own suite. M11.c task 6.3.
#
# ONE UNIT SUITE, and it exists because of a defect: the particle renderer's SPIR-V is compiled by
# hand and CHECKED IN (see shaders/embed_spirv.py), and it imports `cy/frame.slang`'s per-view
# block. When that block gained four rows in the middle, nothing noticed that the embedded module
# still read the old offsets. `unit.particle_modules` walks every embedded module's own layout
# decorations and compares them with `FrameViewData`, the C++ half, so the next change to the block
# that forgets a regeneration is red on every machine — no device, no Slang compiler.
#
# Declared with cy_add_test(), deferred for the reason src/vfx/tests/CMakeLists.txt gives.
set_property(GLOBAL PROPERTY CY_PARTICLES_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_particles_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_PARTICLES_TESTS_DIR)
    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_unit)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_unit)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/rendering/particles/tests: the 'unit' entry of the test taxonomy could not be read "
            "from tests/CMakeLists.txt.")
    endif()
    set(CY_TEST_BUDGET_unit "${budget}")
    set(CY_TEST_TIMEOUT_unit "${timeout}")

    cy_add_test(
        NAME particle_modules
        KIND unit
        SOURCES "${tests_dir}/test_particle_modules.cpp"
        DEPENDENCIES cy::rendering-particles)
    target_include_directories(cy_test_unit_particle_modules
                               PRIVATE "${tests_dir}/../shaders")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_particles_declare_tests)
