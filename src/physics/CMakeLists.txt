# src/physics/ — layer 4: the physics ECS bridge. Components become bodies, the stepper runs in the
# `Physics` stage, and what the solver produced is written back to `cy::scene::LocalTransform`.
# M8.a section 4.
#
# WHY THIS DIRECTORY EXISTS, IN THE WORDS OF THE FILE THAT ASKED FOR IT.
# src/servers/physics/include/cy/servers/physics/components.h:
#
#   "A component is two things: a layout, and a registration in a world. The LAYOUT ... belongs
#   beside the server whose vocabulary it is written in ... The REGISTRATION belongs with whoever
#   owns the world. `cy::scene::LocalTransform` ... is layer 4, and that directory is layer 2, so
#   the bridge that reads `RigidBody`, creates a body and writes `LocalTransform` back cannot live
#   there. It is `src/physics/` at layer 4, mirroring `src/rendering/scene/`, and it does not exist
#   yet."
#
# M4's gate flagged the absence, M7 did not close it, and M8.a's proposal names it as one of the
# four things it verified missing rather than assumed. This is that module.
#
# WHAT DEPENDS ON WHAT, AND WHY THE LIST LOOKS LIKE THIS:
#
#   cy::ecs                 layer 1 — the world, its queries, its schedule and its stages
#   cy::scene               layer 4 — `LocalTransform`, `WorldTransform` and `SceneTree`, which are
#                                     the scene layer's and are not restated here
#   cy::servers-physics     layer 2 — `PhysicsServer`, `PhysicsStepper`, and the component LAYOUTS
#   cy::core-determinism    layer 0 — `SimulationClock` and `StateSchema`
#
# THERE IS NO cy::physics-jolt AND THERE MUST NOT BE. The bridge is handed a `PhysicsServer&`; it
# creates neither the server nor the world. `physics` requires that swapping the backend changes no
# gameplay, and a layer-4 module that named one backend would be the layer that decides which — see
# bridge.h, "WHAT THIS DOES NOT DO, DELIBERATELY". `-D CY_PHYSICS=OFF` therefore changes nothing
# about this module: it is built, tested and green with the reference backend, which is the whole
# argument src/servers/physics/README.md makes for keeping that backend alive.
#
# LAYER. `scene` (4), the same rung src/gameplay/ and src/rendering/scene/ declare and for the same
# reason: above the ECS and the servers, below the loop that drives them.
cy_add_module(
    NAME cy_physics_bridge
    LAYER scene
    ALIAS cy::physics-bridge
    TYPE STATIC
    SOURCES
        src/bridge.cpp
        src/components.cpp
        src/state_schema.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-determinism cy::core-math cy::core-memory
                        cy::core-values cy::ecs cy::scene cy::servers-physics
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
