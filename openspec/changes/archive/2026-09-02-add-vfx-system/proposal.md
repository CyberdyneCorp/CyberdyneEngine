# Add the VFX system: a GPU-first, compiler-driven particle and effects framework

## Why

Particles are currently one requirement inside `rendering-geometry-and-resources`, describing a
conventional module-pipeline particle system. That is adequate for a Unity-class particle system
and inadequate for what this engine targets.

Visual effects are one of the few subsystems where an engine can still differentiate. Physics,
text, and audio have mature libraries that make building your own hard to justify; VFX does not —
the leading implementations (Niagara, VFX Graph) are engine-internal precisely because the system
must be co-designed with the renderer, the GPU scene, and the frame budget. That co-design is
exactly what this engine is positioned to do.

The target is not "a particle system". It is a programmable VFX framework where GPU simulation is
the default rather than an advanced mode, effect graphs are *compiled* rather than interpreted,
effects share one global simulation world rather than each becoming an isolated dispatch, and cost
is bounded by a frame-time budget rather than a quality preset. Those choices are cheap to make
now and extremely expensive to retrofit.

## What Changes

- **New `vfx-system` capability**, replacing the particle requirement in
  `rendering-geometry-and-resources`. **BREAKING** relative to the existing spec: the module
  pipeline described there is superseded by the graph-and-compiler model.
- **GPU-first simulation.** Compute simulation is the default path, designed for millions of
  particles. A CPU path exists for capability fallback and for the small set of effects that
  genuinely need CPU-visible results, with documented lower budgets.
- **A VFX compiler.** Effect graphs compile through a typed IR — attribute analysis, dead-code
  elimination, constant folding, kernel fusion — into Slang, then through the engine's existing
  shader pipeline. Graphs are never interpreted at runtime.
- **Compiler-determined particle layouts.** Attribute storage is structure-of-arrays, and the set
  of attributes is derived from what the graph actually reads and writes, so unused attributes
  cost no bandwidth.
- **A unified simulation world with a global scheduler** that merges compatible emitters into
  shared dispatches, rather than one dispatch per effect instance.
- **Data interfaces** exposing engine data (depth, normals, SDF, physics, terrain, skeletal
  meshes, audio spectrum, wind, ECS queries) to graphs as first-class, typed sources.
- **GPU scene integration** so mesh particles publish instances directly into the renderer's
  GPU-side instance representation, without ECS entities or CPU round trips.
- **GPU-to-GPU events** so an effect can spawn from another effect's collisions without a CPU
  readback, with a hard spawn budget guarding against feedback loops.
- **Frame-budget-driven scalability.** Effects declare an importance class; a budget controller
  adjusts spawn rates, simulation frequency, collision quality, and LOD to hold a GPU time target.
- **Simulation frequency decoupled from rendering**, with interpolation, so distant effects
  simulate at a fraction of the frame rate.
- **A hard determinism boundary**: VFX is explicitly non-deterministic and SHALL NOT influence
  gameplay state, so it cannot break physics determinism, network reconciliation, or replay.
- Fluids are **explicitly deferred**, with the attachment seams reserved.

Non-goals for this change: fluid solvers, offline simulation import (Houdini), hair and fibre
rendering, and voxel rendering. Each is recorded as deferred.

## Capabilities

### New Capabilities

- `vfx-system` — the VFX asset model, compiler, simulation, data interfaces, events, scalability,
  renderers, authoring, and diagnostics.

### Modified Capabilities

- `rendering-geometry-and-resources` — remove the superseded particle systems requirement.
- `rendering-architecture` — name the **GPU scene** as the shared GPU-side instance
  representation, so VFX and GPU-driven culling have a defined thing to publish into.
- `shader-system` — state that engine-generated kernels, including compiled VFX graphs, use the
  same authoring, reflection, caching, and hot-reload path as hand-written shaders.
- `thirdparty-dependencies` — record the VFX runtime as engine-built.
- `build-system-and-platforms` — add `CY_VFX` to the feature options.

## Impact

- **Dependencies**: none added. Noise functions are implemented in-shader from published
  algorithms rather than integrated as a library; the shader toolchain is the existing Slang path.
- **Interfaces**: introduces `VfxServer`, the data interface contract, and the GPU scene
  publication interface. Adds a VFX stage to the frame schedule.
- **Renderer**: VFX becomes a producer for the GPU scene and a consumer of depth, normals, and
  the scene SDF; async compute overlap is expected where the device supports it.
- **Tooling**: adds a VFX graph editor and a VFX cooking step to the asset pipeline.
- **Risk**: this is the largest single subsystem specified so far and the one with the most
  cross-system coupling. The compiler and the global scheduler are where that risk concentrates.
