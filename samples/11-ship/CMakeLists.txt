# samples/11-ship — M11.d's closing artefact. Section 8.
#
# A TWELFTH SHAPE, and it is the first one in this tree that is a PACKAGED PROJECT. `samples/` held
# fifteen entries before this directory and not one of them was built, cooked, packaged, installed
# and launched; every earlier artefact reads its content off a path in the source tree or generates
# it in the process that draws it.
#
#   cy_sample_ship   the program: it opens an INSTALLATION, verifies it, reads its provenance and
#                    its card out of the manifest in force, composes the card, opens a window, and
#                    PRESENTS it on a swapchain.
#   ship.py          the recipe's driver: cook, package, install, launch, and check what came back.
#   just run-ship    the single recipe task 8.1 asks for.
#
# ================================================================================================
# WHAT THIS TARGET LINKS THAT NOTHING ELSE IN samples/ DOES, AND WHY EACH IS THE POINT
# ================================================================================================
#
#   cy::build-graph              the installation. samples/06-open-world links it too, and for the
#                                same reason — a sample reading its content through the manifest in
#                                force rather than off a path is the arrangement M6 built and the
#                                one this rung ships.
#   cy::platform-desktop-sdl3    a window. Every graphics sample before this one drew offscreen.
#   cy::platform-headless        the other display server, chosen at RUN time through the same
#                                interface. Two implementations behind one interface is the whole
#                                of `core-platform-abstraction`'s claim, and a binary that can only
#                                be built against one of them has not tested it.
#   cy::rendering-graph          the frame. The swapchain image is IMPORTED into the graph and the
#                                present layout is DECLARED, so the transition is derived like
#                                every other one — `GraphExecutor`'s `wait_acquire` and
#                                `signal_present` have existed since M3 and this is their first
#                                caller anywhere in the repository.
#
# ================================================================================================
# THE NATIVE PLATFORM BACKEND IS LINKED WHERE IT EXISTS, AND REPORTED ABSENT WHERE IT DOES NOT
# ================================================================================================
#
# Task 8.2 asks this artefact to draw "through the native platform backend section 4 is building,
# and through SDL3". Section 4's `platform/linux-native/` — `LinuxPlatform` and `X11DisplayServer`,
# Xlib and XRandR with no SDL beneath them — is linked HERE IF IT EXISTS and not otherwise, because
# it is a target that is itself conditional on libX11 being present. Where it is absent this sample
# still builds, still runs the SDL3 leg, and reports the native leg ABSENT with that reason on its
# own face rather than inventing a stand-in for it.
#
# What is worth noticing is how little of the sample changes between the two legs: one `case` in
# `DisplayServers::start` and one `#include`. Everything after it is written against
# `cy::DisplayServer&` and cannot tell which implementation answered — which is the claim
# `core-platform-abstraction`'s Complete cell rests on, made by a program rather than by a document.
#
# ================================================================================================
# THE BUILD TOOLS ARE WHAT SHIP THIS SAMPLE'S CONTENT
# ================================================================================================
#
# With `CY_BUILD_TOOLS=OFF` there is no `cy::build-graph`, and an artefact that reads an
# installation has no installation to read. The target is NOT DECLARED and the configure says so in
# one line — the same answer samples/06-open-world gives, and for the same reason.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_subdirectory(ios)
    return()
endif()

if(NOT TARGET cy::build-graph)
    message(STATUS
        "samples/11-ship: the M11.d artefact launches from an installation and needs "
        "cy::build-graph, which CY_BUILD_TOOLS=OFF excludes. It is not built.")
    return()
endif()

cy_add_module(
    NAME cy_sample_ship
    LAYER tools
    TYPE EXECUTABLE
    ALIAS cy::sample-ship
    SOURCES
        main.cpp
        card.cpp
        present.cpp
        "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
    PRIVATE_DEPENDENCIES
        cy::build-graph
        cy::platform-desktop-sdl3
        cy::platform-headless
        cy::rendering-graph
        cy::rhi
        cy::rhi-null)

target_include_directories(cy_sample_ship
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                   "${CMAKE_SOURCE_DIR}/tests/render")

# The Vulkan backend is a LINK-time choice, not a compile-time one — samples/10-world's
# CMakeLists.txt argues the arrangement. With it off, this program still builds, still opens the
# installation, still composes the card, still opens a window, and reports that it had no backend
# that could create a swapchain. That is a report; it is not a stub.
if(CY_RENDERER_VULKAN)
    target_link_libraries(cy_sample_ship PRIVATE cy::rhi-vulkan)
    target_compile_definitions(cy_sample_ship PRIVATE CY_SHIP_HAS_VULKAN=1)
endif()

# The native platform backend, on the same terms: a link-time choice. `platform/linux-native/`
# declares nothing at all on a machine with no libX11, so this asks whether the target exists rather
# than whether an option is on — the option is not this sample's to know about.
if(TARGET cy::platform-linux-native)
    target_link_libraries(cy_sample_ship PRIVATE cy::platform-linux-native)
    target_compile_definitions(cy_sample_ship PRIVATE CY_SHIP_HAS_NATIVE_PLATFORM=1)
endif()

if(NOT CY_BUILD_TESTS)
    return()
endif()

# --- The artefact as a test, and what it needs ---------------------------------------------------
#
# `ship.py` runs `cy_build` and this binary as separate processes and needs python3 to do it. Where
# python3 is absent the entry is not declared and the test list is visibly shorter — what
# samples/06-open-world, 05-editor-session and 05b-agent-authoring all do rather than registering a
# test that would skip on exactly the machines least able to judge the claim.
find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "samples/11-ship: python3 is absent, so smoke.ship is not declared.")
    return()
endif()

add_test(NAME unit.ios_device_runner
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ios/run_device.py" --self-test
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(unit.ios_device_runner PROPERTIES LABELS "unit" TIMEOUT 30)

get_directory_property(CY_SHIP_TIMEOUT DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                       DEFINITION CY_TEST_TIMEOUT_smoke)
if(NOT CY_SHIP_TIMEOUT)
    message(FATAL_ERROR
        "samples/11-ship: the test taxonomy's timeouts could not be read from "
        "tests/CMakeLists.txt. If those variables were renamed, this lookup should move with them.")
endif()

# THE SMOKE ENTRY RUNS THE WHOLE RECIPE — cook, package, install, launch — WITH `--platform
# headless`. That is deliberate and it is the honest half: a hosted runner has no display, and a
# CTest entry that needed one would be a test that only ever ran here. What it proves is the four
# acts that do not need a device — the package verifies, the provenance is there, the card comes out
# of the manifest by logical name, and the program reports the presentation leg NOT EVALUATED rather
# than claiming it. The leg that DRAWS is `just run-ship`, which a person runs, and which is what
# produced docs/design/images/m11d-ship.png.
add_test(NAME smoke.ship
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ship.py"
                 --binaries "$<TARGET_FILE:cy_sample_ship>" "$<TARGET_FILE:cy_build>"
                 --work "${CMAKE_CURRENT_BINARY_DIR}/run"
                 --platform headless
                 --frames 1
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(smoke.ship PROPERTIES LABELS "smoke" TIMEOUT "${CY_SHIP_TIMEOUT}")
