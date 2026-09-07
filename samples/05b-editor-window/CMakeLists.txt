# samples/05b-editor-window — the M5.5 artefact a person drives. Task 4.1.
#
# --- WHY THIS DIRECTORY DECLARES NO cy_add_module() TARGET ----------------------------------------
#
# The same reason samples/05-editor-session declares none: what is demonstrated is a window being
# operated, across two processes, by synthesised X11 input. A C++ driver would link no engine code
# and prove nothing about the engine. `window.py` is the artefact and `just run-editor-window` is
# the recipe; this file declares the one CTest entry that runs it.
#
# `just run-sample editor-window` does NOT work: that recipe finds `samples/*/cy_sample_<name>` and
# there is no such binary here.
#
# --- WHY THIS ONE IS NOT IN THE PERMANENT GATE SET, AND ITS SIBLING IS -----------------------------
#
# It needs an X server with XTEST, a graphics device that can import a dma-buf with an explicit DRM
# format modifier, python-xlib and Pillow. A hosted continuous-integration runner has none of those,
# and `delivery-roadmap` is explicit that a criterion which cannot be evaluated on the host running
# it must be reported as not evaluated rather than as passed. So the ledger declares it with
# `requires = "display"` and a reason, and the CI job that stands for it is the agent artefact next
# door, which needs neither. `design.md` §5: two artefacts, because there are two audiences.

if(NOT CY_BUILD_TESTS)
    return()
endif()

# --- Where the artefact cannot run, and why that is an absence rather than a skip ------------------
#
# THIS ARTEFACT IS ABOUT A WINDOW. A run on a machine with no display would say nothing about
# docking, the palette or keyboard-first operation, so it is not declared there and the test list is
# visibly shorter — the position bindings/swift/, samples/04-character/ and tests/render/ all take.
#
# The display is checked TWICE and deliberately: here, so a machine that never had one does not
# register the entry at all; and in `window.py`, which exits 3 when the display it was given turns
# out not to be usable at run time. `SKIP_RETURN_CODE` below makes that exit a reported SKIP rather
# than a pass — a configure-time answer cannot speak for a test-time environment, and a check that
# reported green because the window never opened is the exact failure this milestone exists to
# correct.
if(NOT UNIX OR APPLE)
    message(STATUS "samples/05b-editor-window: the driver synthesises input through X11's XTEST "
                   "extension, which this platform does not have; it is not registered")
    return()
endif()
if(NOT DEFINED ENV{DISPLAY} AND NOT DEFINED ENV{WAYLAND_DISPLAY})
    message(STATUS "samples/05b-editor-window: there is no display on this machine, and this "
                   "artefact is about a window; it is not registered")
    return()
endif()

find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
find_program(CY_WINDOW_CARGO cargo)
find_program(CY_WINDOW_JUST just)
if(NOT Python3_Interpreter_FOUND OR NOT CY_WINDOW_CARGO OR NOT CY_WINDOW_JUST)
    message(STATUS
        "samples/05b-editor-window: the M5.5 window artefact needs python3, cargo and just; it is "
        "not registered.\n"
        "   python3: ${Python3_Interpreter_FOUND}  cargo: ${CY_WINDOW_CARGO}  "
        "just: ${CY_WINDOW_JUST}")
    return()
endif()

# python-xlib drives the window and Pillow reads the pixels back. Checked at configure so the entry
# is absent rather than failing later with an import error, which reads as a defect in the editor.
execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import Xlib.ext.xtest, PIL"
                RESULT_VARIABLE CY_WINDOW_PY_MODULES OUTPUT_QUIET ERROR_QUIET)
if(NOT CY_WINDOW_PY_MODULES EQUAL 0)
    message(STATUS
        "samples/05b-editor-window: python-xlib (with XTEST) and Pillow are not installed, so a "
        "key press cannot be synthesised and the window cannot be photographed; it is not "
        "registered. Install them with: python3 -m pip install python-xlib pillow")
    return()
endif()

set(CY_WINDOW_PROFILE "")
foreach(candidate IN LISTS CY_PROFILES)
    if(CY_PROFILE_${candidate}_CONFIG STREQUAL CMAKE_BUILD_TYPE)
        set(CY_WINDOW_PROFILE "${candidate}")
    endif()
endforeach()
if(NOT CY_WINDOW_PROFILE)
    message(FATAL_ERROR
        "samples/05b-editor-window: no workflow profile maps to configuration "
        "'${CMAKE_BUILD_TYPE}'. cmake/profiles.cmake owns that table.")
endif()

# The smoke timeout, read from the taxonomy that owns it rather than restated. This session opens a
# real window, waits for a compositor and drives it at human speed, so it is the slowest smoke entry
# in the tree; if it starts timing out, the taxonomy is where the number is decided.
get_directory_property(CY_WINDOW_TIMEOUT DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                       DEFINITION CY_TEST_TIMEOUT_smoke)
if(NOT CY_WINDOW_TIMEOUT)
    message(FATAL_ERROR
        "samples/05b-editor-window: the test taxonomy's timeouts could not be read from "
        "tests/CMakeLists.txt. If those variables were renamed, the lookup should move with them.")
endif()

add_test(NAME smoke.editor_window
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/window.py"
                 --build --profile "${CY_WINDOW_PROFILE}"
                 --work "${CMAKE_CURRENT_BINARY_DIR}/session"
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(smoke.editor_window PROPERTIES
                     LABELS "smoke" TIMEOUT "${CY_WINDOW_TIMEOUT}"
                     SKIP_RETURN_CODE 3
                     RUN_SERIAL TRUE)
