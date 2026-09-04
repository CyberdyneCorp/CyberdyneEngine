# src/rendering/graph/tests/ — the render graph's suites. Section 2.2 of the M3 tasks.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing, and the taxonomy is read back out
# of tests/ rather than restated, because a budget copied into a second file is a budget that drifts.
#
#   render_graph        the derivation, at unit cost: declarations, culling, the hazard rules, the
#                       coalescer, async compute and the single-queue fold. Every case compiles a
#                       plan with no device at all — the derivation makes no device calls, which is
#                       what makes these cases cheap enough to be unit tests.
#
#   render_graph_scale  the two suites whose cases do not fit a millisecond, measured rather than
#                       guessed:
#
#                       test_aliasing.cpp   a sixteen-step chain of 1024x1024 transients, whose
#                                           placement is quadratic in the number of transients by
#                                           design (each one is tested against every placement made
#                                           so far). That is the measurement task 7.3 needs and it
#                                           is not a case that should be shrunk to fit a budget.
#                       test_execution.cpp  every case builds a null device — fifteen handle pools
#                                           and a format table — and two of them build one twice to
#                                           compare runs.
#
# Shrinking either to fit a millisecond would be testing something else.
set_property(GLOBAL PROPERTY CY_RENDER_GRAPH_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_render_graph_declare_tests)
    get_property(cy_graph_tests_dir GLOBAL PROPERTY CY_RENDER_GRAPH_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/graph/tests: the '${kind}' entry of the test taxonomy could not be "
                "read from tests/CMakeLists.txt. If the budget variables were renamed, this lookup "
                "and the deferral around it should go away — declare the suites from tests/unit/ "
                "instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME render_graph
        KIND unit
        SOURCES
            "${cy_graph_tests_dir}/test_async_compute.cpp"
            "${cy_graph_tests_dir}/test_barriers.cpp"
            "${cy_graph_tests_dir}/test_culling.cpp"
            "${cy_graph_tests_dir}/test_declaration.cpp"
        DEPENDENCIES cy::rendering-graph cy::rhi-null)

    cy_add_test(
        NAME render_graph_scale
        KIND integration
        SOURCES
            "${cy_graph_tests_dir}/test_aliasing.cpp"
            "${cy_graph_tests_dir}/test_execution.cpp"
        DEPENDENCIES cy::rendering-graph cy::rhi-null)

    # The Vulkan suite exists only when the Vulkan backend does. `smoke`, because a case here
    # creates a real device, submits real work and waits for the GPU: `testing-and-quality` puts
    # anything that touches hardware above the unit and integration budgets, and 30 s is the
    # taxonomy's own ceiling for one.
    #
    # It SKIPS rather than fails on a machine with no GPU — loudly, naming the backend that was
    # selected instead — because most continuous integration machines have none and a suite that
    # failed there is a suite somebody disables. The null backend covers the same graph headlessly,
    # which is the whole point of having been written first.
    if(CY_RENDERER_VULKAN)
        get_directory_property(smoke_budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_smoke)
        get_directory_property(smoke_timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_smoke)
        set(CY_TEST_BUDGET_smoke "${smoke_budget}")
        set(CY_TEST_TIMEOUT_smoke "${smoke_timeout}")
        cy_add_test(
            NAME vulkan_frame
            KIND smoke
            SOURCES "${cy_graph_tests_dir}/test_vulkan_frame.cpp"
            DEPENDENCIES cy::rendering-graph cy::rhi-null cy::rhi-vulkan)
    endif()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_render_graph_declare_tests)
