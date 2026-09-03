# platform/host/ — layer 3: the desktop host's frame loop.
#
# Task 3.4.3, design.md §3. The runtime exposes tick(); the `while (running)` is here, so a platform
# that drives frames itself is a different host and not a different runtime.
#
# This target does not depend on cy::runtime and cannot: layer 3 never links layer 5. run_host_loop()
# takes any callable, and the caller — the sample, or a future platform shell — is what binds it to
# Runtime::tick(). Nothing SDL is used, so the loop is the same on a headless run as on a windowed
# one.

cy_add_module(
    NAME cy_platform_host
    LAYER platform
    ALIAS cy::platform-host
    SOURCES src/host_loop.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-platform)
