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

# CyberGeometry: the virtualised geometry path. The cooked asset and its pages, the cluster
# hierarchy and its crack-free error metric, the GPU geometry cache and its streaming feedback, GPU
# traversal and cluster culling, and the visibility buffer with material resolve. M7 section 7.
#
# Layer 4 rather than layer 2, which is where `virtual-texturing` sits: this module dispatches
# compute and owns a render target, and a server may name neither. Its own CMakeLists.txt carries
# the argument.
add_subdirectory(virtual_geometry)

# GPU-driven culling's dispatch: the compute pass `cpu_reference_cull` was written to be checked
# against, and the first module in this tree to create a compute pipeline. M7 task 5.1. Separate from
# culling/ because that module names no device, no graph and no shader — which is what lets its whole
# suite run headless — and this one names all three.
add_subdirectory(gpu_culling)

# CyberTexture's device half: the feedback buffer a shader writes, the GPU resolve that compacts it
# without a per-pixel stream reaching the CPU, the page table a shader samples, and the two-subsystem
# residency composition that arbitration exists for. M7 tasks 4.1, 4.2 and 4.3.
# src/servers/render/virtual_texturing/README.md names this module in advance and says what it owes.
add_subdirectory(virtual_texturing)

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

# CyberShadow: the virtual shadow address space, receiver-driven pages, clipmaps and their snapping,
# the page cache, precise invalidation, update classes, the shadow budget, derived bias and the
# fallback chain. Tasks 8.1 and 8.2. After lighting/ because the conventional atlas and cascades are
# the fallback this module degrades to, and neither depends on the other.
add_subdirectory(shadows)

# CyberRay: the renderer service that owns acceleration structures — bottom-level lifecycle and
# refit policy, the top level, a geometry adapter per geometry source, the ray query interface, the
# build budget, and the capability gate. Task 9.4. Before gi/ because the hardware tracing tier is a
# consumer of this interface and not the other way round; it names no device, for the reason its own
# CMakeLists.txt gives.
add_subdirectory(raytracing)

# CyberDenoiser: the ONE accumulation and edge-aware filter every stochastic signal goes through —
# indirect diffuse, indirect specular, ray-traced shadows, ambient occlusion and stochastic direct
# lighting. Task 9.3. Above raytracing/ only because neither depends on the other; gi/ depends on
# both, which is the point — per-signal denoisers are how a renderer acquires four filters that
# ghost differently.
add_subdirectory(denoise)

# CyberGI: the GI scene, the surface and radiance caches, the distance field, the screen, software
# and hardware tracing tiers with their sample confidence, the resolve, reflections on the same
# infrastructure, the probes, the bake and the GI budget. Tasks 9.1, 9.2 and 9.5. Last, because it
# consumes raytracing/ as one tier and denoise/ as its reconstruction.
add_subdirectory(gi)

# CyberTemporal: ONE temporal framework — jitter, derived motion vectors, history, invalidation and
# reprojection — consumed by everything stochastic rather than reimplemented per effect. Task 8.3.
# Before post/ because the post chain's antialiasing and upscaling stages are two of its consumers.
add_subdirectory(temporal)

# The renderer's one budget arbiter, the subsystem controllers it allocates to, the four named
# renderer profiles and the rendering configuration asset. M7 tasks 10.1 and 10.2, and the module
# that takes `rendering-architecture` to Complete: six paged or stochastic systems arrived in this
# milestone and each of them can spend an unbounded amount of a frame. It depends on none of them —
# a subsystem declares a priced ladder and reports its own cost — which is what lets the control law
# be certified over 71 load magnitudes instead of over one screenshot.
add_subdirectory(arbiter)

# The post-process chain in its defined order: AO, subsurface, fog, reflections, the temporal stage,
# exposure, depth of field, motion blur, bloom, tonemap, grading, the display-space effects and the
# output encoding. Task 8.4. Last, because it consumes temporal/ and is the end of the frame.
add_subdirectory(post)
