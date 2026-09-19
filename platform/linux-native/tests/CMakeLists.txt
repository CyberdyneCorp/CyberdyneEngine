# platform/linux-native/tests/ — the native Linux backend's suite. M11.d task 4.1.
#
# Declared with cy_add_test(), deferred to the end of the top-level directory's processing and
# reading the taxonomy back out of tests/ — the same arrangement platform/desktop-sdl3/tests/ uses
# and for the same reason: a suite declared from a layer-3 directory cannot see the budgets that
# tests/CMakeLists.txt defines until that directory has been processed.
#
#   linux_native   `integration`, not `unit`: it spawns real children, opens a real X connection and
#                  sends real X events. The Platform half needs no display and runs everywhere,
#                  including a CI container; the DisplayServer half SKIPS LOUDLY where there is no
#                  X server, because a suite that failed there is a suite somebody disables.
#
# THE CASE TO READ is "the input source sees the display server's events without stealing them". An
# X connection has ONE event queue, and a second XNextEvent loop would consume the events the
# display server needed — the failure looks like "input works when the window is idle and stops when
# it is being resized". That is the same defect sdl3_input_source.h describes, reached by a
# different route, and the only reason this backend can be trusted with both is that the case exists.

set_property(GLOBAL PROPERTY CY_LINUX_NATIVE_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set_property(GLOBAL PROPERTY CY_LINUX_NATIVE_X11_INCLUDE "${X11_INCLUDE_DIR}")

function(cy_linux_native_declare_tests)
    get_property(cy_linux_native_tests_dir GLOBAL PROPERTY CY_LINUX_NATIVE_TESTS_DIR)
    get_property(cy_linux_native_x11_include GLOBAL PROPERTY CY_LINUX_NATIVE_X11_INCLUDE)

    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_integration)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_integration)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "platform/linux-native/tests: the 'integration' entry of the test taxonomy could not be "
            "read from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
            "the deferral around it should go away — declare the suite from tests/integration/ "
            "instead.")
    endif()
    set(CY_TEST_BUDGET_integration "${budget}")
    set(CY_TEST_TIMEOUT_integration "${timeout}")

    cy_add_test(
        NAME linux_native
        KIND integration
        SOURCES "${cy_linux_native_tests_dir}/test_linux_native.cpp"
        DEPENDENCIES cy::platform-linux-native ${X11_LIBRARIES})

    # The suite includes <X11/Xlib.h> to push a real X event, which is legitimate under platform/
    # and nowhere else — the same permission platform/desktop-sdl3/tests/ has for <SDL3/SDL.h>.
    target_include_directories(cy_test_integration_linux_native PRIVATE "${cy_linux_native_x11_include}")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_linux_native_declare_tests)
