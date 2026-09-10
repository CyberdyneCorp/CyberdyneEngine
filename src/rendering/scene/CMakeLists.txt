# src/rendering/scene/ — layer 4: the extract stage. The ECS world becomes a render snapshot.
# Tasks 4.1.2 and 4.1.4.
#
# `rendering-architecture`: "Rendering SHALL consume an immutable render snapshot published at a
# defined point each frame, not live ECS storage", and design.md §4: "Nothing walks the node tree at
# render time. The renderer's input is the GPU scene; the snapshot that fills it is taken at M2's
# commit boundary."
#
# WHY THIS IS A SEPARATE MODULE FROM src/servers/render/, WHICH OWNS THE SNAPSHOT TYPE. The render
# server is layer 2 and MAY NOT KNOW ABOUT ENTITIES — `rendering-architecture` says so in the first
# sentence of its first requirement, and the layer check enforces it. So the server owns what a
# snapshot IS and how a scene consumes one, and this module owns where one COMES FROM: an ECS world,
# a scene tree's transforms, and the commit boundary that says when. The split is the requirement
# made structural, and it is what lets `RenderServer` be tested with no world in existence.
#
# WHAT DEPENDS ON WHAT, AND WHY THE LIST LOOKS LIKE THIS:
#
#   cy::ecs                 layer 1 — the world, its queries and its change detection
#   cy::scene               layer 4 — `WorldTransform` and `InterpolatedTransform`, which are the
#                                     scene layer's and are not restated here
#   cy::servers-render      layer 2 — `RenderSnapshot`, and the value vocabulary it is written in
#   cy::core-determinism    layer 0 — `CommitObserver`, `StateSchema` and `Classified<>`
#
# There is no cy::rhi and no cy::rendering-graph: extraction runs on the simulation thread at the
# commit boundary and has no business naming a device. That is not a rule this file states, it is
# one the dependency list makes true.
# --- The reflection metadata for this module's components -------------------------------------------
#
# M8.b's task 11.3, and the same arrangement src/scene/ uses: src/core/reflect/CMakeLists.txt owns
# the generator invocation — one run covers the whole tree because the identity manifest is shared —
# and it writes this module's output HERE, because a generated translation unit includes the header
# it describes and a layer-0 file may not include <cy/rendering/scene/components.h>.
#
# Listed explicitly like every other source, and committed like every other generated artefact, so a
# checkout without libclang still builds and an identity change is a reviewable diff.
set(cy_rendering_scene_generated_sources
    generated/cy_reflect_generated_rendering.cpp
    generated/cy/rendering/scene/components.reflect.cpp)

cy_add_module(
    NAME cy_rendering_scene
    LAYER rendering
    ALIAS cy::rendering-scene
    TYPE STATIC
    SOURCES
        ${cy_rendering_scene_generated_sources}
        src/asset_binding.cpp
        src/components.cpp
        src/extract.cpp
        src/node_templates.cpp
        src/state_schema.cpp
    PUBLIC_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-determinism cy::core-math cy::core-memory
                        cy::core-reflect cy::core-values cy::ecs cy::scene cy::servers-render)

# The generator runs from src/core/reflect/, which the top-level adds before this directory, so the
# target exists by now. Without this edge a build with CY_REFLECT_GENERATE on could compile last
# run's committed metadata while the generator was still rewriting it.
if(TARGET cy_core_reflect_generate)
    add_dependencies(cy_rendering_scene cy_core_reflect_generate)
endif()

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
