# src/rendering/sky_illumination/tests/ — the seam, run. M11.c tasks 2.1–2.4.
#
# Declared through the deferral src/rendering/gi/tests/ uses: cy_add_test() is defined by
# tests/CMakeLists.txt, which the top-level file adds after src/, so the declaration waits until the
# end of the top-level directory's processing and the taxonomy is read back out of tests/ rather
# than restated.
#
#   rendering_gi_sky   INTEGRATION, and it could not be anything else: every case drives a real
#                      illumination system over a real distance field, and the cloud-shadow case
#                      drives a real `environment::FieldStore` with real tiles written into it.
#                      What it does NOT need is a device, which is what lets the seam
#                      `m11c:gi-sky-term-constructed` is about be judged on a machine with no GPU.
set_property(GLOBAL PROPERTY CY_SKY_ILLUMINATION_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_sky_illumination_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_SKY_ILLUMINATION_TESTS_DIR)

    foreach(kind integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/sky_illumination/tests: the '${kind}' entry of the test taxonomy "
                "could not be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME rendering_gi_sky
        KIND integration
        SOURCES "${tests_dir}/test_gi_sky.cpp"
        DEPENDENCIES cy::rendering-sky-illumination)
    # support.h is included by relative path from the source above, which is compiled from the
    # top-level binary directory — so this directory has to be on the include path, exactly as
    # tests/render/ does for its own probe headers.
    target_include_directories(cy_test_integration_rendering_gi_sky PRIVATE "${tests_dir}")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_sky_illumination_declare_tests)
