# platform/linux-native/ — layer 3: a NATIVE Platform and DisplayServer for Linux. M11.d task 4.1.
#
# THE SECOND IMPLEMENTATION, WHICH IS THE POINT. `core-platform-abstraction` has had exactly one
# implementation of each interface since M0 and both are SDL3's, so nothing has ever proved the
# abstraction carries no SDL assumption. This module is the proof, and the exit criterion is a diff:
# adding it required no change in src/core/, src/ecs/, src/servers/ or src/scene/. See README.md,
# which records what the port found.
#
# WHY LINUX AND NOT macOS, and why X11 and not Wayland, are both measured rather than preferred —
# design.md §4 for the first, include/cy/platform/x11_display_server.h for the second.
#
# NO FEATURE OPTION GATES IT. Rule 3 of every milestone brief since M3: a delivered capability is on
# by default, or nothing builds it. This is built whenever the host is Linux and refuses to configure
# when the X11 development packages are absent, naming them — which is what every Linux leg of
# ci.yml already installs (libx11-dev, libxrandr-dev) and what this machine has. A backend behind an
# option nobody sets is a backend nothing compiles, which is the position CY_RENDERER_VULKAN was
# moved out of at M3.
#
# X11 AND XRANDR ARE SYSTEM LIBRARIES AND ARE NOT IN deps/manifest.toml. That file is "the single
# source of truth for third-party code the build ACQUIRES AND LINKS" through FetchContent, and its
# parser requires a git repository and a 40-character commit for every entry; a system library has
# neither, and an entry for one would carry fabricated fields and would then be fetched. The
# precedent is already in the tree: platform/desktop-sdl3/CMakeLists.txt links `psapi` on Windows
# the same way. A platform's own system libraries are part of what a platform backend IS.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "platform/linux-native is Linux-only and platform/CMakeLists.txt should not have added it "
        "on a '${CMAKE_SYSTEM_NAME}' host. A native backend for another desktop is a directory "
        "beside this one, not an #ifdef inside it (core-platform-abstraction, 'New platform port').")
endif()

find_package(X11 QUIET)
if(NOT X11_FOUND OR NOT X11_Xrandr_FOUND)
    message(FATAL_ERROR
        "platform/linux-native needs the X11 and XRandR development packages and this host has "
        "X11_FOUND=${X11_FOUND} X11_Xrandr_FOUND=${X11_Xrandr_FOUND}.\n"
        "  Install them:  sudo apt-get install -y libx11-dev libxrandr-dev\n"
        "  Every Linux job in .github/workflows/ci.yml already does, so a red configure here means "
        "a machine that is behind that list rather than a platform this backend cannot support.")
endif()

cy_add_module(
    NAME cy_platform_linux_native
    LAYER platform
    ALIAS cy::platform-linux-native
    SOURCES
        src/linux_platform.cpp
        src/x11_display_server.cpp
        src/x11_input_source.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-platform cy::servers-input
    # PRIVATE, so that no X11 include directory is inherited by anything that links this module.
    # That is the same link-time guarantee `cy::dep::jolt` and `cy::dep::miniaudio` rely on, and
    # tools/layercheck's `windowlib` check is the source-level half of it — one without the other is
    # a rule that can be turned off silently.
    PRIVATE_DEPENDENCIES ${X11_LIBRARIES} ${X11_Xrandr_LIB} ${CMAKE_DL_LIBS})

target_include_directories(cy_platform_linux_native PRIVATE ${X11_INCLUDE_DIR})

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
