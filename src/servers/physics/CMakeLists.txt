# src/servers/physics/ — layer 2: the engine-owned physics interface, and the reference backend
# that was written before Jolt existed. Section 4.2 of the M4 tasks.
#
# `physics` — "Engine-owned physics interface": `PhysicsServer` is engine-defined and handle-based,
# and "the rest of the engine SHALL depend only on this interface, never on backend types". Layer 2
# is what makes that structural: src/backends/ is layer 3, so nothing in this directory CAN name a
# Jolt type, and the layer checker fails the build over an attempt.
#
# WHAT BELONGS HERE. The interface and its vocabulary — handles, shapes, bodies, filters, queries,
# events, constraints, the tuning block, capabilities and statistics. The engine-owned algorithms
# that are written over the interface rather than inside a backend: the character controller
# (design's reasoning is in character.h — a character that walks differently after a backend swap
# has changed gameplay), the fixed-step integration on M2's simulation clock, the determinism probe
# and the 2D API.
#
# WHAT DOES NOT. A solver (src/backends/physics-jolt/), an ECS world, a scene node, a render server.
# `PhysicsStepper` publishes transforms into a `TransformSink` the caller implements precisely
# because `cy::scene::LocalTransform` is layer 4 and unreachable from here.
#
# NOT GATED BY CY_PHYSICS, and that is deliberate for the same reason src/abi/ is not gated: an
# interface that exists in some configurations has a test suite that runs in some configurations.
# CY_PHYSICS gates the JOLT BACKEND and the Jolt fetch — the option that costs something — and this
# module plus its reference backend compile in every build and every profile.
#
# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_servers_physics
    LAYER servers
    ALIAS cy::servers-physics
    TYPE STATIC
    SOURCES
        src/body.cpp
        src/character.cpp
        src/components.cpp
        src/constraints.cpp
        src/debug.cpp
        src/determinism.cpp
        src/events.cpp
        src/physics2d.cpp
        src/queries.cpp
        src/shapes.cpp
        src/stepper.cpp
        src/types.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    # cy::core-determinism is here rather than private because `hash_state()` takes a
    # `StateHashTree&` and `PhysicsStepper::tick()` takes a `SimulationClock&` — both are in the
    # interface, so a consumer inherits them.
    PUBLIC_DEPENDENCIES cy::core-base cy::core-determinism cy::core-math cy::core-memory
                        cy::core-values)

# The reference backend, first (design.md §4) and always built: it is the last link in the backend
# fallback chain and the reason a physics test runs with -D CY_PHYSICS=OFF.
add_subdirectory(reference)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
