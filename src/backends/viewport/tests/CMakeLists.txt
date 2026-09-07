# src/backends/viewport/tests/ — the wire, on a machine with no GPU.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines and which does not exist while
# this file is read; the declaration is deferred to the end of the top-level directory's processing,
# exactly as src/servers/render/tests/ does and for the same reason.
#
# `unit`, not `render`: nothing here creates a device. The seqlock, the two-flag reservation, the
# slot rule and the refusals are decisions, and they are the parts of this transport that cost the
# most to get wrong — so they are checked where every machine can check them, and the Vulkan is left
# to `smoke.editor_window`, which needs a display anyway.
set_property(GLOBAL PROPERTY CY_VIEWPORT_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_viewport_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_VIEWPORT_TESTS_DIR)
    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_unit)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_unit)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/backends/viewport/tests: the 'unit' entry of the test taxonomy could not be read "
            "from tests/CMakeLists.txt.")
    endif()
    set(CY_TEST_BUDGET_unit "${budget}")
    set(CY_TEST_TIMEOUT_unit "${timeout}")

    get_directory_property(integration_budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_integration)
    get_directory_property(integration_timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_integration)
    set(CY_TEST_BUDGET_integration "${integration_budget}")
    set(CY_TEST_TIMEOUT_integration "${integration_timeout}")

    cy_add_test(
        NAME viewport_publisher
        KIND unit
        SOURCES "${tests_dir}/test_wire.cpp"
        DEPENDENCIES cy::backends-viewport)

    # The two contended protocols, in the suite their cost puts them in. Both run two threads for
    # tens of thousands of iterations, which is what it takes to hit the races they exist to find —
    # the editor's own spike measured them at a few per cent of frames.
    cy_add_test(
        NAME viewport_publisher
        KIND integration
        SOURCES "${tests_dir}/test_concurrency.cpp"
        DEPENDENCIES cy::backends-viewport)

    # wire.h is a private header of the module: it is an implementation detail everywhere except
    # here, where it is the subject. Reaching it from the tests rather than publishing it is what
    # keeps `Ring` and `AnnouncementPage` out of every consumer's include path.
    find_package(Threads REQUIRED)
    foreach(target cy_test_unit_viewport_publisher cy_test_integration_viewport_publisher)
        target_include_directories(${target} PRIVATE "${tests_dir}/../src")
        target_link_libraries(${target} PRIVATE Threads::Threads)
    endforeach()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_viewport_declare_tests)
