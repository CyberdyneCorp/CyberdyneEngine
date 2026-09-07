# src/rendering/raytracing/tests/ — the ray tracing infrastructure suite. Task 9.4.
#
# Declared through the same deferral src/rendering/culling/tests/ uses: cy_add_test() is defined by
# tests/CMakeLists.txt, which the top-level file adds after src/, so the declaration waits until the
# end of the top-level directory's processing and the taxonomy is read back out of tests/ rather
# than restated.
#
#   render_raytracing        the adapter table, structure lifecycle, the build budget, eviction, the
#                            query, and the capability gate. Every case builds a real structure and
#                            traces a real ray: the subject is the join between the top level, the
#                            bottom level and the instance transform, and a mock of any of them
#                            would test the mock.
#   render_raytracing_scale  the thousand-instance sharing scenario, at integration cost. It is here
#                            rather than in the unit suite because a top-level build over a thousand
#                            instances costs more than 1 ms of CPU in a Debug build, and shrinking
#                            the number to fit would be shrinking the requirement.
set_property(GLOBAL PROPERTY CY_RENDER_RAYTRACING_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_render_raytracing_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_RENDER_RAYTRACING_TESTS_DIR)

    foreach(kind unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/raytracing/tests: the '${kind}' entry of the test taxonomy could "
                "not be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME render_raytracing
        KIND unit
        SOURCES
            "${tests_dir}/test_acceleration.cpp"
            "${tests_dir}/test_capability.cpp"
        DEPENDENCIES cy::rendering-raytracing)

    cy_add_test(
        NAME render_raytracing_scale
        KIND integration
        SOURCES "${tests_dir}/test_scale.cpp"
        DEPENDENCIES cy::rendering-raytracing)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_render_raytracing_declare_tests)
