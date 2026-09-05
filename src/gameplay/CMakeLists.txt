# src/gameplay/ — CyberGameplay: lifetimes, control, and the one validated command stream.
# Section 4.4 of the M4 tasks.
#
# ================================================================================================
# READ THE DEPENDENCY LIST BEFORE YOU EXTEND IT
# ================================================================================================
#
# `cy::servers-input` IS NOT THERE AND MUST NOT BE ADDED. design.md §3: "Input reaches simulation
# **only** as commands. There is no 'read the input state in a system' path." Because this module
# declares no dependency on the input server, its headers are not on a gameplay translation unit's
# include path at all — a gameplay system cannot include one even by accident.
#
# src/gameplay/tests/test_bypass.cpp asserts exactly that, with `__has_include`, and it fails the
# moment the dependency is added. That test also demonstrates *why*: it runs a conforming simulation
# and a bypassing one, replays both from the command log, and shows the bypassing one diverging.
# design.md §3 again: "a single system that reads a device directly ... makes the M9 guarantees
# unachievable until someone finds and removes it, and nothing will point at it, because everything
# will appear to work until a desync months later."
#
# The bridge from input actions to gameplay commands therefore lives **above** both modules — in the
# runtime or in the sample — where it can see the input server and the command stream at once. That
# is the whole architectural content of the invariant: the translation is a thing somebody wrote, in
# one place, rather than a capability every system quietly has.
#
# LAYER. `scene` (4): above cy::ecs (1) and the servers (2), below cy::runtime (5). There is no
# `gameplay` layer in `engine-architecture`'s table and inventing one would be a change to
# cmake/modules.cmake, which is not this milestone's to make; `scene` is the correct existing rung —
# it is where the modules that compose ECS data into game-facing structure already sit.
#
# HEADLESS IS A REQUIREMENT, NOT A CONFIGURATION. `gameplay-framework`: "fully functional with no
# renderer, no audio, no interface, and no GPU ... a gameplay system that requires a camera,
# viewport, material, or audio device SHALL be a defect." None of those is reachable from this
# dependency list, so the dedicated server links no rendering code because there is none to link.
cy_add_module(
    NAME cy_gameplay
    LAYER scene
    ALIAS cy::gameplay
    TYPE STATIC
    SOURCES
        src/command.cpp
        src/context.cpp
        src/control.cpp
        src/validation.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-determinism cy::core-math cy::core-memory
                        cy::core-values cy::ecs
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
