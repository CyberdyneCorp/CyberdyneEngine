# platform/desktop-sdl3/ — layer 3: Platform and DisplayServer for Linux, Windows and macOS, over
# SDL3.
#
# Tasks 3.2.2, 3.2.3, 3.2.4, 3.3.2, 3.3.3, 3.3.5 and 3.3.6. design.md §4: SDL3 sits beneath the
# engine-owned interfaces for all three desktop platforms, and a native backend for one of them is
# an M11 task. No SDL type appears above this directory; tools/layercheck/ fails the build if one
# does.
#
# HOST FILES. Everything SDL3 does not provide — the executable path, the per-process memory
# figures, the crash handler, and the native window behind a surface — is one file per operating
# system under src/host/, selected here. That is task 3.2.4's rule: platform-specific code lives in
# a file of its own, never behind an #ifdef in a shared one. A fourth desktop platform is a fourth
# file and one branch below.

set(cy_sdl3_sources
    src/sdl3_display_server.cpp
    src/sdl3_platform.cpp)

set(cy_sdl3_link_libraries "")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND cy_sdl3_sources src/host/host_linux.cpp src/host/host_signals_posix.cpp)
elseif(APPLE)
    list(APPEND cy_sdl3_sources src/host/host_macos.cpp src/host/host_signals_posix.cpp)
elseif(WIN32)
    list(APPEND cy_sdl3_sources src/host/host_windows.cpp src/host/host_signals_windows.cpp)
    # GetProcessMemoryInfo(). Everything else this backend calls is in kernel32.
    list(APPEND cy_sdl3_link_libraries psapi)
else()
    message(FATAL_ERROR
        "platform/desktop-sdl3 supports Linux, Windows and macOS; this host is "
        "'${CMAKE_SYSTEM_NAME}'.\n"
        "  A new platform adds one file under src/host/ and one branch here, and implements "
        "Platform, DisplayServer, an input backend, an audio backend and a surface provider — with "
        "no change in src/core/ or above (core-platform-abstraction, 'New platform port').")
endif()

cy_add_module(
    NAME cy_platform_desktop_sdl3
    LAYER platform
    ALIAS cy::platform-desktop-sdl3
    SOURCES ${cy_sdl3_sources}
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PUBLIC_DEPENDENCIES cy::core-platform
    PRIVATE_DEPENDENCIES cy::dep::sdl3 ${cy_sdl3_link_libraries})
