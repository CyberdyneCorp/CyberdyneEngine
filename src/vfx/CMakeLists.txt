# src/vfx/ — CyberVFX. M8.c section 2, `vfx-system`.
#
# ================================================================================================
# TWO TARGETS, AND THE SPLIT IS A REQUIREMENT RATHER THAN A PREFERENCE
# ================================================================================================
#
# `vfx-system`: "The runtime SHALL contain no graph compiler" and "the runtime SHALL contain no
# graph interpreter and no graph-to-source compiler". A promise in a comment is not that; two
# targets are.
#
#   cy::vfx-compiler   the asset model, the VFX IR, the derived attribute layout, the data
#                      interface registry and the Slang emitter. It links `cy::graph` and NOTHING
#                      ELSE above cy::core — no device, no world, no renderer — because a compiler
#                      that needed one of those is a compiler that cannot run in a cook. Nothing in
#                      a shipping runtime links it.
#
#   cy::vfx            the simulation world, the shared particle pool, the global scheduler, the
#                      budget controller, the event channels and the bounded readback. It links the
#                      compiler target for the COMPILED ARTEFACT'S TYPES — `CompiledSystem`,
#                      `VfxKernel`, `AttributeLayout` — and calls no function that reads a
#                      `cy::graph::Graph`. `test_vfx_runtime.cpp` asserts the runtime never sees an
#                      authored graph by construction: it has no way to name one.
#
# ================================================================================================
# WHAT cy::vfx DEPENDS ON, AND WHY EACH ONE IS THERE
# ================================================================================================
#
#   cy::ecs                    THE DETERMINISM FIREWALL. `<cy/ecs/firewall.h>`'s `WriteScope` is
#                              opened around the readback delivery path, because `vfx-system`
#                              requires development builds to "detect and report attempts to write
#                              replicated or physics-owned components from VFX-driven code paths"
#                              and this milestone's section 1 built the enforcement point. VFX does
#                              not enforce the rule; it declares its origin so the rule can.
#   cy::rendering-particles    THE SEAM TO THE RENDERER. `ParticleInstance` is a camera-relative
#                              position, a size and a linear radiance, and it is the whole of what
#                              the simulation publishes. This target does NOT link the frame, the
#                              render graph or the assembly: a simulation that had to know how a
#                              frame is assembled would be a second renderer, which is the thing
#                              M8.c exists to stop.
#
# ================================================================================================
# LAYER AND OPTION
# ================================================================================================
#
# LAYER scene (4), the rung `src/graph/`, `src/world/`, `src/gameplay/` and `src/camera/` all chose
# for the same reason: `engine-architecture`'s table has no `vfx` rung and inventing one is a change
# to cmake/module.cmake that four modules have already declined to make.
#
# CY_VFX gates this whole directory, which is what `vfx-system` asks for: "The system SHALL be
# removable at build time via CY_VFX without affecting the rest of the renderer." `-D CY_VFX=OFF`
# excludes both targets and their suites and nothing else changes, because nothing below layer 4
# names either of them. src/CMakeLists.txt is where the `if(CY_VFX)` is.
cy_add_module(
    NAME cy_vfx_compiler
    LAYER scene
    ALIAS cy::vfx-compiler
    TYPE STATIC
    SOURCES
        src/asset.cpp
        src/compile.cpp
        src/emit_slang.cpp
        src/gpu_layout.cpp
        src/interfaces.cpp
        src/ir.cpp
        src/layout.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PUBLIC_DEPENDENCIES
        cy::core-base
        cy::core-memory
        cy::core-values
        cy::graph)

cy_add_module(
    NAME cy_vfx
    LAYER scene
    ALIAS cy::vfx
    TYPE STATIC
    SOURCES
        src/budget.cpp
        src/events.cpp
        src/renderers.cpp
        src/runtime.cpp
        src/world.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES
        cy::core-base
        cy::core-math
        cy::core-memory
        cy::ecs
        cy::rendering-particles
        cy::vfx-compiler)

# gpu/ is a THIRD target and its own directory. See gpu/CMakeLists.txt for why it cannot be part of
# either target above: it names a device, a render graph and a shader front end, and the property
# that makes `cy::vfx`'s "no second renderer" claim checkable is that `cy::vfx` names none of them.
add_subdirectory(gpu)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
