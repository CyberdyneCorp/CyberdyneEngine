# src/runtime/ — layer 5: bootstrap, subsystem wiring, and Runtime::tick().
#
# Tasks 3.4.1 and 3.4.2. The runtime never owns a loop: the `while (running)` is the host's, under
# platform/, and this target links no host and no backend. It consumes the interfaces in
# src/core/platform/ and the trace in src/core/diagnostics/, and nothing else exists yet for it to
# wire.
#
# cy::core-platform is PUBLIC because Platform& and DisplayServer& are in the runtime's own
# interface; cy::core-diagnostics is PRIVATE because the trace is something the runtime writes to,
# not something a consumer of the runtime has to know about.
#
# cy::core-config is PUBLIC for the same reason as cy::core-platform: RuntimeConfig carries a
# cy::config::ModuleRegistry*, so a host that populates one names the type. That registry is what
# makes the four module stages of `engine-architecture`'s startup sequence do something (task 4.5).

# M2 (tasks 4.1.x and 4.2.x) filled three of the eleven startup stages and made the tick loop real,
# so the target grew four sources and four dependencies. Each dependency is PUBLIC because each
# appears in the runtime's own interface: `RuntimeConfig` names a `ServerRegistry` and a
# `Simulation`, and a `Simulation` names an `ecs::World`, a `scene::SceneTree` and a
# `determinism::SimulationClock`. cy::core-jobs is there because `RuntimeConfig::jobs` is a
# `jobs::JobSystem*` — the runtime hands it to the schedule and never submits work itself.
#
# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_runtime
    LAYER runtime
    ALIAS cy::runtime
    SOURCES
        src/features.cpp
        src/frame_commands.cpp
        src/runtime.cpp
        src/servers.cpp
        src/simulation.cpp
        src/state_hash.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES
        cy::core-platform
        cy::core-config
        cy::core-determinism
        cy::core-jobs
        cy::ecs
        cy::scene
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

# The startup-order probe. It starts and stops a runtime, prints the journal, and exits — which is
# the only way to compare the order across *processes* rather than across iterations of one loop.
# tests/smoke/test_startup_order.cpp runs it a hundred times (task 3.4.4).
#
# Behind CY_BUILD_TESTS: it exists for that test and a shipping tree has no use for it.
if(CY_BUILD_TESTS)
    cy_add_module(
        NAME cy_runtime_startup_probe
        LAYER runtime
        TYPE EXECUTABLE
        SOURCES probe/startup_order_probe.cpp
        PRIVATE_DEPENDENCIES cy::runtime cy::platform-headless cy::platform-desktop-sdl3)

    # The tick-loop probe. It runs a real Runtime with a Simulation attached in fixed-step mode and
    # prints the committed point, the state version and the state hash — which is the only way to
    # compare a hash across *processes* rather than across iterations of one loop.
    # tests/test_tick_loop.cpp runs it twenty times (tasks 4.1.2 and 4.2.6).
    cy_add_module(
        NAME cy_runtime_tick_loop_probe
        LAYER runtime
        TYPE EXECUTABLE
        SOURCES probe/tick_loop_probe.cpp
        PRIVATE_DEPENDENCIES cy::runtime cy::platform-headless cy::platform-desktop-sdl3)
endif()

# tests/ is read now and declares its suites later; see the comment at the top of it for why.
if(CY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
