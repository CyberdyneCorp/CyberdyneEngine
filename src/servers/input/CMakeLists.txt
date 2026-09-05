# src/servers/input/ — layer 2: CyberInput. Section 4.1 of the M4 tasks.
#
# `input-and-actions` draws the boundaries and this module is the middle one: the platform layer
# produces normalised, timestamped device events; this capability turns them into semantic actions;
# and `gameplay-framework` turns actions into commands.
#
# WHY THE DEPENDENCY LIST IS THIS SHORT, AND WHAT IT BUYS. Layer 2 is below src/backends/ (3),
# platform/ (3), src/scene/ (4) and src/runtime/ (5). So there is no SDL here, no ECS world, no
# scene node, no camera and no renderer — the layer checker fails the build over any of them. Three
# of the specification's requirements stop being rules somebody has to remember and become
# properties of the build:
#
#   * "an input layer that writes a transform" has crossed a line — it cannot, there is no
#     transform reachable from this layer;
#   * "a reference-frame modifier SHALL NOT depend on renderer internals" — `set_reference_frame()`
#     takes three vectors, and there is no camera type to depend on;
#   * "Input SHALL function with no devices present" — nothing here can open a device, so the
#     headless path is the only path and the tests exercise it by default.
#
# The converse boundary is enforced from the other side: src/gameplay/ declares no dependency on
# this module, so a gameplay translation unit cannot include these headers at all. design.md §3.
cy_add_module(
    NAME cy_servers_input
    LAYER servers
    ALIAS cy::servers-input
    TYPE STATIC
    SOURCES
        src/action.cpp
        src/binding.cpp
        src/device.cpp
        src/diagnostics.cpp
        src/event.cpp
        src/frame.cpp
        src/profile.cpp
        src/scheme.cpp
        src/server.cpp
        src/types.cpp
        src/user.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::core-values
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
