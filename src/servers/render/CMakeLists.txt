# src/servers/render/ — layer 2: the handle-based render server. Section 4.1 of the M3 tasks.
#
# `rendering-architecture` — the `RenderServer` owns all renderable state behind generational
# handles and has no knowledge of entities, nodes or scripts. That last clause is what puts it at
# layer 2, and layer 2 is BELOW src/backends/ (3) and src/rendering/ (4).
#
# WHAT THAT MEANS FOR THE DEPENDENCY LIST BELOW, AND WHY IT IS SHORT. There is no cy::rhi here and
# there cannot be: this module may not name a device, a command buffer, an image format the RHI
# declared, or a render graph. Everything it holds is expressed in engine types — cy::core-math's
# `Transform` and `Aabb`, cy::core-values' `Handle` and `Name`, cy::core-memory's `HandlePool`. The
# modules above (src/rendering/) own the device and consume this one.
#
# WHAT BELONGS HERE. The object families and their handles; the scene, view and instance model; the
# GPU scene as a publication interface; the simulation-to-render snapshot; mesh representation and
# vertex compression; the deterministic sort key; debug draw; frame and view statistics. M5 adds the
# editor's two entry points — engine-side picking against the draw list a view produced, and the
# viewport transport's publishing endpoint — which are here rather than in the editor because
# `editor-viewport-and-gizmos` requires picking to run engine-side and forbids a second renderer.
#
# WHAT DOES NOT. A pass, a pipeline, a barrier, an upload, an ECS query, a node. Extraction from the
# ECS is src/rendering/scene/'s, because it needs an ECS world and this module is forbidden one —
# which is the requirement being enforced by the build rather than remembered.
cy_add_module(
    NAME cy_servers_render
    LAYER servers
    ALIAS cy::servers-render
    TYPE STATIC
    SOURCES
        src/debug_draw.cpp
        src/gizmo.cpp
        src/gpu_scene.cpp
        src/mesh.cpp
        src/model.cpp
        src/picking.cpp
        src/server.cpp
        src/snapshot.cpp
        src/sort.cpp
        src/statistics.cpp
        src/types.cpp
        src/viewport_transport.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-determinism cy::core-math cy::core-memory
                        cy::core-values
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

# --- M6's two additions, as submodules rather than as more sources above -------------------------
#
# `rendering-culling-and-lod` reaches Working and `rendering-geometry-and-resources` reaches Complete
# at M6, and both grew enough surface to be their own libraries: the GPU culling records and the
# depth pyramid in culling/, and skinning, blend shapes, dynamic geometry and the GPU memory ledger
# in geometry/. They are separate targets rather than more entries in the SOURCES list above for one
# concrete reason — a consumer that wants the instance model should not link the depth pyramid, and
# `cy_add_module`'s dependency graph is what makes that visible in a review.
#
# Both are layer 2 like their parent, both name no device, and both depend on cy::servers-render
# because they read the records it publishes.
add_subdirectory(culling)
add_subdirectory(geometry)

# CyberTexture, in its own module beside this one rather than inside it. M6 section 5.
#
# It depends on cy::servers-residency and this module deliberately does not: the render server is
# what every module above reaches for the scene model, and a transitive dependency on the residency
# policy from here would put that policy in every translation unit that draws anything. The
# subdirectory's own CMakeLists.txt argues the rest.
add_subdirectory(virtual_texturing)
