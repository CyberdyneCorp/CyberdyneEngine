# src/gameplay/play/ — layer 4: spawning, and the play session that simulates an authored world.
# M8.a section 5.
#
# ================================================================================================
# WHY THIS IS A SEPARATE TARGET FROM cy::gameplay AND NOT THREE MORE FILES IN IT
# ================================================================================================
#
# Read src/gameplay/CMakeLists.txt's own header first: "READ THE DEPENDENCY LIST BEFORE YOU EXTEND
# IT". `cy_gameplay` deliberately depends on `cy::ecs` and the core and NOTHING ELSE — that is what
# makes `tests/test_bypass.cpp`'s `__has_include` check meaningful, and the same property is what
# lets a dedicated server link no rendering code "because there is none to link".
#
# A play session needs three things that module does not have: `cy::scene` for the tree it spawns
# into, `cy::scene-serialization` for the authored `.cyworld` it simulates, and `cy::physics-bridge`
# for the bodies. Adding those to `cy_gameplay` would put four modules' headers on the include path
# of a suite whose whole argument is about what is NOT reachable from it.
#
# So the dependency goes the other way: this target depends on `cy::gameplay` and the module above
# keeps its short list exactly as it was. The bypass invariant is untouched — `cy::servers-input` is
# not here either, and a play session reaching an input device would be the same defect one layer
# up.
#
# WHAT DEPENDS ON WHAT:
#
#   cy::gameplay             layer 4 — the session and command vocabulary a play session belongs to
#   cy::scene                layer 4 — `SceneTree`, `LocalTransform`, transform propagation
#   cy::scene-serialization  layer 4 — `.cyworld`, read and written by the engine (section 1)
#   cy::physics-bridge       layer 4 — the components, the bodies and the `Physics` stage
#   cy::ecs, cy::core-*      below    — the world, the schedule and the clock
#
# There is no `cy::physics-jolt` and no renderer: `gameplay-framework` requires a game to be "fully
# functional with no renderer, no audio, no interface, and no GPU", and a play session is the first
# thing in this engine that could plausibly have wanted one. Its whole suite runs headless.
cy_add_module(
    NAME cy_gameplay_play
    LAYER scene
    ALIAS cy::gameplay-play
    TYPE STATIC
    SOURCES
        src/driver.cpp
        src/launcher.cpp
        src/mode.cpp
        src/session.cpp
        src/spawn.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-determinism cy::core-math cy::core-memory
                        cy::core-platform cy::core-values cy::ecs cy::gameplay cy::physics-bridge
                        cy::scene cy::scene-serialization
    PRIVATE_DEPENDENCIES cy::core-assets cy::core-diagnostics)

# ================================================================================================
# THE SECOND RUNTIME PROCESS
# ================================================================================================
#
# `live-editing` (Play modes) gives `SeparateProcess` a runtime location — "A second runtime
# process" — and `editor-architecture` ("Standalone play") requires that selecting it LAUNCH one.
# Until the repair round that added this target, `PlayModeSupport::runtime_launcher` was a literal
# `true` beside a comment saying the build carried no launcher, and the suite that claimed to drive
# "a world in each of the three modes" built three sessions in ONE process. This is the process the
# claim was about.
#
# IT IS NOT BEHIND CY_BUILD_TESTS, and that is the difference between a probe and a product. The
# runtime probes in src/runtime/ exist for one test each and a shipping tree has no use for them;
# this binary IS separate-process play. A build that omitted it would report the mode unavailable —
# `runtime_launcher_available()` asks the filesystem — which is an honest answer to give about a
# tree that chose not to build its own runtime, and not one to give about a shipping engine.
#
# WHAT IT LINKS IS THE ISOLATION CLAIM. "Editor-only state SHALL be absent from the runtime" is not
# a boolean on a capability table; it is this list. There is no cy::abi, no editor bridge and no
# editor services here, and the editor itself is a Rust application in another workspace. Editor
# state is absent because there is no expression in this binary that could reach any.
#
# LAYER runtime rather than tools: it is the engine running a world, not something that consumes the
# engine to produce an artefact. Layer 5 is also what lets it link the SDL3 platform at layer 3,
# which it needs for one thing only — to measure its own `PlayModeSupport` the same way the launcher
# did, against its own directory, instead of asserting that being launched proves a launcher.
#
# RUNTIME_OUTPUT_DIRECTORY is the build root, because that is where the executables that launch it
# are: `RuntimeProcess` resolves the host "beside the calling executable", and the same rule has to
# hold in a build tree and in an installed layout or the resolution would need to know which it is
# looking at.
cy_add_module(
    NAME cy_play_runtime_host
    LAYER runtime
    TYPE EXECUTABLE
    ALIAS cy::play-runtime-host
    SOURCES host/main.cpp
    PRIVATE_DEPENDENCIES cy::core-assets cy::core-memory cy::gameplay-play cy::physics-reference
                         cy::platform-desktop-sdl3 cy::scene cy::scene-serialization)

set_target_properties(cy_play_runtime_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
