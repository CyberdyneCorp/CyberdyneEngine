# src/ — the engine layer stack. Subdirectories are added in layer order, low to high, so that a
# target's dependencies are always already declared when it is.
#
# Only the directories that contain targets are added; adding an empty directory would only hide
# which layers actually exist. servers/ gained its first target at M3 and abi/ at M4, and both are
# added below.
add_subdirectory(core)

# core/serialize and core/determinism are layer 0 and belong in src/core/CMakeLists.txt's list
# beside the other core modules; they are registered here because that file was not this
# milestone's to edit. Moving the two lines there is the correct next edit to it, and nothing else
# has to change when they move.
add_subdirectory(core/serialize)
add_subdirectory(core/determinism)

# M2's own layers. ecs/ is layer 1 and scene/ is layer 4; scene/ reads its own serialization/
# subdirectory, so the four M2 modules are declared by these three lines.
add_subdirectory(ecs)

# M3's own layers. backends/ is layer 3 and opens with the RHI and its null backend; rendering/ is
# layer 4 and holds the render graph that computes barriers over that interface. backends/ is read
# before scene/ and rendering/ because both of those are above it, and a target's dependencies are
# always already declared when it is.
add_subdirectory(backends)

# servers/ is layer 2 and opens with the render server (M3 section 4.1). It is read before
# backends/ would need it — nothing at layer 2 may depend on layer 3 — and before rendering/, which
# consumes it.
add_subdirectory(servers)

# text/ is the Unicode layer above CyberText and M8.b section 9.4: the bidirectional algorithm, line
# breaking, Arabic joining, the shaping cache and localisation — the three algorithms
# src/servers/text/'s `TextCapabilities` declares the minimal backend cannot do. Layer 2, a peer of
# src/servers/text/ rather than something above it, and it depends on cy::core alone.
add_subdirectory(text)

# save/ is layer 2 and arrives at M6 (section 6). It is read after servers/ because it sits at the
# same layer, and before scene/ and everything above, all of which may consume it: the persistence
# overlay is the model world partition maintains and a save encodes. It depends on layer 0 alone —
# see src/save/CMakeLists.txt for why the layer is 2 rather than the layer of its first consumer.
add_subdirectory(save)

add_subdirectory(scene)
add_subdirectory(rendering)

# graph/ is CyberGraph and M8.b section 2: the shared authoring layer, the shared pure-expression
# SSA core, and one lowering per consumer. It declares LAYER scene (4) for the reason
# src/gameplay/CMakeLists.txt and src/world/CMakeLists.txt both give — there is no `graph` rung in
# `engine-architecture`'s table and inventing one is not this milestone's change to make — and it
# depends on nothing above cy::core, because a graph compiler that needed a scene, a device or a
# server would be a compiler that cannot run in a cook. Read after rendering/ and before the
# consumers that lower through it.
add_subdirectory(graph)

# animation/ is CyberAnimation and M8.b section 5: skeletons and bone level of detail, clips and
# their codec, the runtime of the compiled pose program, root motion, the constraint framework,
# retargeting, the GPU pose world and pose sharing. Layer 4, read after graph/ because it consumes
# `cy::graph::pose`'s compiled program and before runtime/. Behind CY_ANIMATION, which the
# specification requires the runtime to be removable by — `-D CY_ANIMATION=OFF` excludes this
# directory and nothing else changes, because nothing below layer 4 names it.
if(CY_ANIMATION)
    add_subdirectory(animation)
endif()

# navigation/ and ai/ are M8.b section 6. Both declare LAYER scene (4) for the reason graph/ above
# and world/ below both give. navigation/ is read first because ai/ depends on it — `ai-system`
# requires agents to "drive movement through the navigation system rather than implementing their
# own pathfinding", and cmake/features.cmake encodes the same edge as CY_AI|CY_NAVIGATION. Both are
# read after graph/, because a compiled behaviour program is `cy::graph::behaviour`'s and the AI
# runtime is what executes it.
add_subdirectory(navigation)
if(CY_AI)
    add_subdirectory(ai)
endif()

# physics/ is the physics ECS bridge and M8.a section 4: it registers the components
# src/servers/physics/ declares the layouts for, creates bodies from them, drives `PhysicsStepper`
# in the `Physics` stage and writes `cy::scene::LocalTransform` back. Layer 4, mirroring
# rendering/scene/ — which is where src/servers/physics/include/cy/servers/physics/components.h
# said it would be, in those words, at M4. It is read after scene/ because it consumes `SceneTree`,
# and before world/ and runtime/, both of which may drive it.
add_subdirectory(physics)

# world/ is CyberWorld: the spatial and persistence layer above the ECS, and M6 section 3. It is
# read after scene/ because it declares LAYER scene (4) — there is no `world` rung in
# `engine-architecture`'s table and src/gameplay/CMakeLists.txt already declined to invent one — and
# before runtime/, which drives its tick.
add_subdirectory(world)

# gameplay/ is layer 4 and arrives at M4 (section 4.4). It is read after scene/ and rendering/ and
# before runtime/, which is where it sits in the stack: above the ECS and the servers, below the
# loop that drives them. Its dependency list is short on purpose and the omission is the point —
# see src/gameplay/CMakeLists.txt, and design.md §3.
add_subdirectory(gameplay)

# ml/ is CyberML and M8.c section 4: tensors, model assets, sessions, the backend abstraction, the
# per-frame budget and the determinism boundary. LAYER servers (2) — it depends on cy::core and
# cy::ecs and nothing above, which is what lets a session be created from a server, from a scene
# system or from a cook. It is nevertheless read HERE, after gameplay/, rather than beside save/ at
# its own layer: its second target, cy::ml-cook, declares LAYER scene and depends on cy::gameplay,
# and this file's own rule is that "a target's dependencies are always already declared when it is".
# cy::ml itself is depended on by nothing, so reading it late costs nothing.
#
# cy::ml-cook is a separate target because `<cy/gameplay/cook_firewall.h>` is at layer 4 and a
# layer-2 module may not name it — see src/ml/CMakeLists.txt.
#
# Behind CY_ML, which `ml-inference` requires CyberML to be removable by: "CyberML SHALL be
# removable at build time via CY_ML, and the engine SHALL be fully functional without it." The
# option fetches nothing — the inference RUNTIME is behind CY_ML_ONNXRUNTIME — and nothing below
# layer 4 names this directory, so `-D CY_ML=OFF` excludes it and nothing else changes. src/ai/
# does not depend on it, which is `ml-inference`'s "AI without ML" scenario as a link-graph fact.
if(CY_ML)
    add_subdirectory(ml)
endif()


# camera/ is CyberCamera and M8.b section 7: rigs compiled to programs by cy::graph::camera, the
# stack that blends them, the batched collision and occlusion dispatch, cuts, render views, the
# listener anchor and the streaming source. Layer 4, the rung graph/, gameplay/ and world/ chose.
# Read after graph/ and world/, which it consumes, and before runtime/, which drives its frame.
# ui/ is CyberUI and M8.b section 9: the element store, the retained tree, layout, `.cyss`, input
# routing, the layer stack, flattening and the accessibility audit. Layer 4, and it deliberately
# does NOT depend on cy::ecs — `ui-system` requires UI elements not to be entities, and the link
# graph is what keeps that true.
# audio/ is CyberAudio's acoustics half and M8.b section 10: the acoustics backend behind which
# Steam Audio sits, its geometry extraction, the asynchronous simulation, the importance tiers and
# voice virtualisation, the effect chain and interactive music. Layer 4, above src/servers/audio/,
# because acoustic geometry and importance scoring both need a world and layer 2 may not name one.
add_subdirectory(audio)

# Behind CY_UI, which `ui-system` requires the system to be removable by: "The system SHALL be
# removable at build time via CY_UI". `-D CY_UI=OFF` excludes this directory and nothing else
# changes, because nothing below layer 4 names it and the only other thing that does is
# samples/08-vertical-slice, which declines to declare itself — the same arrangement CY_ANIMATION
# and CY_AI have above. Until M8.b's closing gate the option gated nothing at all and defaulted off,
# which is the shape the spike found in CY_RENDERER_VULKAN.
if(CY_UI)
    add_subdirectory(ui)
endif()

add_subdirectory(camera)

# sequencing/ is CyberSequence and M8.c section 3: exact time, the authored model, the compiler and
# its interval and event indexes, the compiled program, evaluation into per-subsystem batches,
# arbitration, capture and restore, seeking, skipping and the preload plan — plus two bridges, one
# to the camera stack and one to the gameplay command stream. Layer 4, the rung graph/, gameplay/,
# world/ and camera/ chose. Read after camera/ and gameplay/, which its two bridges consume, and
# before runtime/. The core target depends on cy::core alone, which is what makes "a sequence
# orchestrates and owns nothing" a property of the link graph.
add_subdirectory(sequencing)

# vfx/ is CyberVFX and M8.c section 2: the asset model, the graph compiler and VFX's own IR, the
# compiler-derived attribute layout, GPU-first simulation with a DECLARED CPU fallback, the unified
# simulation world and its global scheduler, data interfaces, GPU events with the bounded readback
# path, and the importance-class budget controller. Layer 4, the rung graph/, world/, gameplay/ and
# camera/ all chose. Read after graph/ — it lowers through CyberGraph — and after rendering/, whose
# `cy::rendering-particles` is the thirty-two-byte seam the simulation publishes into.
#
# Behind CY_VFX, which `vfx-system` requires the system to be removable by: "The system SHALL be
# removable at build time via CY_VFX without affecting the rest of the renderer." `-D CY_VFX=OFF`
# excludes this directory and its suites and nothing else changes, because nothing below layer 4
# names either target and the only other consumer is samples/08-vertical-slice, which declines to
# declare itself — the arrangement CY_ANIMATION, CY_AI and CY_UI already have. Both directions are
# built and tested, which is what M8.b's gate found had never been done for CY_UI.
if(CY_VFX)
    add_subdirectory(vfx)
endif()

add_subdirectory(runtime)

# abi/ is layer 6 and arrives at M4 (section 2). It is read last because it is the top of the
# engine's stack: the C ABI may name the ECS and the core, and nothing in the engine may name it
# back. The comment at the top of this file said this directory would be added when it held a
# target — it does now.
add_subdirectory(abi)
