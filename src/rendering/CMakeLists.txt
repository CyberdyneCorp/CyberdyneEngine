# src/rendering/ — layer 4: the renderer's engine-side code, above the RHI and below the runtime.
#
# M3 opens this layer with the render graph. `rendering-architecture`'s render server is layer 2 and
# lives in src/servers/render/; what belongs here is everything that consumes the RHI and is not a
# backend: the graph, the GPU scene, the passes, the material and mesh representations.
#
# WHY LAYER 4 AND NOT LAYER 2. The render graph calls the device: it creates transient resources,
# records command buffers and emits the barriers it derived. A server is layer 2 and cannot see a
# backend, so a graph at layer 2 would need the RHI injected as an abstract interface it may not
# name — which is the shape you get when a layer number is chosen before the dependency is looked
# at. The graph sits above the backends because it uses one.
add_subdirectory(graph)

# The extract stage: an ECS world and a scene tree become a render snapshot at M2's commit
# boundary. Layer 4 because it reads the scene layer, which is layer 4; it names no device, which is
# what keeps extraction on the simulation thread honest.
add_subdirectory(scene)

# The material model: the BRDF the shaders implement, the GPU material table, the standard
# material, the fallbacks, and validation. Read after graph/ only because these two do not depend on
# each other — the frame's passes, which arrive later in this directory, consume both.
add_subdirectory(material)

# Culling and level of detail: the spatial indices, the per-view visible sets, and the LOD, HLOD and
# shadow-caster decisions. Before forward/ because the frame consumes a visible set; after material/
# only because neither depends on the other.
add_subdirectory(culling)

# Lights, their physical units, the shadow atlas and the directional cascades. The CPU half of
# `rendering-lighting-and-shadows` at Seed: no device, no shader, and the GPU-side light record laid
# out to match cy/light.slang field by field.
add_subdirectory(lighting)

# The frame itself: the cluster grid and its light assignment, the depth prepass, the sorted draw
# list, and the thirteen-stage pass order declared into the render graph. Last of the C++ modules,
# because it consumes every one above it.
add_subdirectory(forward)

# The engine's own Slang source: the shader standard library `shader-system` requires, and the
# shaders the frame is built from. It compiles no C++ — the target stages the tree so a test, a tool
# or the sample reaches the modules through the virtual filesystem rather than through a source
# path. The compiler that consumes them is src/backends/shader/ at layer 3, because a SPIR-V header
# may not appear above the backends.
add_subdirectory(shaders)
