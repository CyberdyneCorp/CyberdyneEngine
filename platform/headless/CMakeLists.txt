# platform/headless/ — layer 3: DisplayServer with no window system.
#
# Task 3.3.4. Required by `core-platform-abstraction` and by CI: every test that is not about a real
# window runs against this backend, and so does `just run-sample empty --headless`.
#
# There is no headless Platform: process services, clocks, paths and subprocesses do not need a
# window system, so the SDL3 Platform serves a headless run unchanged. Only the display side has to
# be replaced.

cy_add_module(
    NAME cy_platform_headless
    LAYER platform
    ALIAS cy::platform-headless
    SOURCES src/headless_display_server.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-platform)
