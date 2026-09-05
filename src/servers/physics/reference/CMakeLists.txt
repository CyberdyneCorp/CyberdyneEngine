# src/servers/physics/reference/ — the physics interface without a physics library.
#
# Task 4.2.1, design.md §4. Written BEFORE the Jolt backend, and that order is the design: with no
# solver to lean on, the interface above it had to be an interface. Written afterwards it would have
# been a set of empty functions shaped by decisions Jolt had already made.
#
# WHY IT IS AT LAYER 2 AND NOT BESIDE JOLT AT LAYER 3. src/backends/ exists to isolate third-party
# APIs, and this target has none — it links `cy::servers-physics` and nothing else. Putting it at
# layer 3 would say there is a library here to keep out of the rest of the engine, and there is not.
# (The RHI's null backend sits at layer 3 because the whole RHI does; physics' interface is at 2.)
#
# Always built, never gated. It is the last link in the backend fallback chain, it is what makes a
# physics test runnable in a configuration with no Jolt, and it is the second implementation that
# keeps `PhysicsServer` an interface rather than a wrapper.
cy_add_module(
    NAME cy_physics_reference
    LAYER servers
    ALIAS cy::physics-reference
    TYPE STATIC
    SOURCES
        src/reference_server.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::servers-physics)
