# platform/desktop-sdl3/tests/ — the SDL3 input event source's suite. Task 4.1.1.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory's processing and
# reading the taxonomy back out of tests/, exactly as src/servers/render/tests/ does.
#
#   sdl3_input   `integration`, not `unit`, because it brings up a real SDL subsystem and pushes
#                real `SDL_Event`s. It needs **no display**: SDL's event subsystem is independent of
#                video, so the whole suite runs headless in CI with no GPU and no X server. That is
#                what makes an end-to-end test of the platform boundary affordable at all.
#
# THE CASE TO READ IS "four transitions in one window arrive as four events". `input-and-actions`
# requires the platform layer to deliver timestamped events **without coalescing transitions**, and
# information destroyed at this layer cannot be recovered above it — see design.md §5. It is the
# platform half of the property src/servers/input/tests/test_fixed_tick.cpp checks from the other
# side.
set_property(GLOBAL PROPERTY CY_SDL3_INPUT_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_sdl3_input_declare_tests)
    get_property(cy_sdl3_tests_dir GLOBAL PROPERTY CY_SDL3_INPUT_TESTS_DIR)

    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_integration)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_integration)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "platform/desktop-sdl3/tests: the 'integration' entry of the test taxonomy could not be "
            "read from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
            "the deferral around it should go away — declare the suite from tests/integration/ "
            "instead.")
    endif()
    set(CY_TEST_BUDGET_integration "${budget}")
    set(CY_TEST_TIMEOUT_integration "${timeout}")

    cy_add_test(
        NAME sdl3_input
        KIND integration
        SOURCES "${cy_sdl3_tests_dir}/test_input_source.cpp"
        DEPENDENCIES cy::platform-desktop-sdl3 cy::dep::sdl3)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_sdl3_input_declare_tests)
