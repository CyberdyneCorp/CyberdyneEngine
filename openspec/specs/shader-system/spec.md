# shader-system Specification

## Purpose

Defines how shaders are authored, compiled, specialised, cached, and bound. CyberdyneEngine does
**not** implement its own shading language: it adopts **Slang** as the authoring language,
compiling to SPIR-V, with SPIRV-Cross producing MSL for the Metal backend.

Writing a bespoke shading language is a multi-year commitment that competes directly with
building the renderer. Slang gives generics, interfaces, modules, automatic differentiation, and
multi-target output, and is designed for exactly this use.

## Requirements

### Requirement: Slang as the authoring language
Engine and user shaders SHALL be written in **Slang** (`.slang`), compiled offline to SPIR-V.

The engine SHALL provide a **shader standard library** of Slang modules: BRDF functions, light
evaluation, cluster lookup, shadow sampling, GI sampling, tonemapping, colour space conversion,
packing and encoding helpers, noise, and sampling patterns.

Material shaders SHALL be authored against a documented **material interface**, implementing a
surface function rather than a whole pipeline, so the engine controls the light loop and pass
structure.

#### Scenario: Material author writes a surface function
- **WHEN** a user writes a custom material
- **THEN** they SHALL implement the surface interface (populate albedo, normal, roughness,
  metallic, emission, and optional per-model outputs) and the engine SHALL supply vertex
  processing, light iteration, and output composition

#### Scenario: Full control when needed
- **WHEN** a user needs a fully custom pass
- **THEN** they SHALL be able to author a complete shader stage against the RHI directly, outside
  the material interface

#### Scenario: Shared code is a module
- **WHEN** several shaders need the same helper
- **THEN** it SHALL live in a Slang module imported by each, with no textual preprocessor
  inclusion

### Requirement: Compilation pipeline
Shader compilation SHALL be an **offline** step producing cooked artefacts:

1. Slang source → Slang compiler → SPIR-V per entry point and permutation
2. SPIR-V → validation and optimisation
3. SPIR-V → reflection (descriptor bindings, push constants, vertex inputs, specialization
   constants, workgroup size)
4. Per backend: SPIR-V retained (Vulkan), or SPIRV-Cross → MSL → Metal library (Metal), or
   SPIR-V → DXIL (D3D12)
5. Package into a **shader library** artefact keyed by content hash

Runtime shader compilation SHALL exist only in development builds, for hot reload.

#### Scenario: Shipping build compiles no shaders from source
- **WHEN** a game ships
- **THEN** it SHALL contain compiled backend-native shader artefacts and no Slang compiler

#### Scenario: Compile error surfaces with source location
- **WHEN** a shader fails to compile
- **THEN** the error SHALL carry the Slang source file, line, and column, and appear in the
  editor's shader editor and the build log

### Requirement: Permutations and specialization
Shader variation SHALL be expressed, in order of preference:

1. **Specialization constants** — resolved at pipeline creation, no separate compilation of
   SPIR-V (sample counts, feature toggles, loop bounds)
2. **Slang generics and interfaces** — compile-time polymorphism producing distinct entry points
   only where genuinely different code is needed
3. **Preprocessor permutations** — last resort, requiring an explicit declaration of the
   permutation axis and its cardinality

Every permutation axis SHALL declare its allowed values so the total permutation count is known
and reportable at build time.

#### Scenario: Permutation explosion is visible
- **WHEN** a shader declares permutation axes whose product exceeds a configured budget
- **THEN** the build SHALL warn with the axis breakdown, before compile times become a problem

#### Scenario: Sample count as a specialization constant
- **WHEN** shadow sample count changes with a quality setting
- **THEN** it SHALL be a specialization constant, so a quality change creates a new pipeline from
  the same SPIR-V rather than a new compilation

### Requirement: Reflection-driven binding
Descriptor set layouts, push-constant ranges, and vertex input layouts SHALL be derived from
shader reflection, not declared separately in C++.

The engine SHALL define a fixed **descriptor set convention** so reflection results are
predictable:

| Set | Contents | Update frequency |
|---|---|---|
| 0 | Global: frame constants, samplers, bindless arrays, shadow atlases, GI resources | Per frame |
| 1 | View: camera matrices, view constants, cluster buffers, per-view targets | Per view |
| 2 | Pass: pass-specific resources | Per pass |
| 3 | Draw: material data and per-draw resources (unused when bindless) | Per draw |

#### Scenario: Layout mismatch is impossible
- **WHEN** a shader's bindings change
- **THEN** the derived layout SHALL change with it, and no separate C++ declaration can drift

#### Scenario: Convention violated
- **WHEN** a shader binds a per-frame resource in set 3
- **THEN** the cook step SHALL fail with a diagnostic naming the convention

### Requirement: Material shader generation
The engine SHALL generate material shader source from a **material definition**: a declarative
description of parameters (with types, defaults, and inspector metadata), texture slots, the
shading model, blend and cull state, and an optional custom surface function body.

Generated shaders SHALL be deduplicated by content hash so materials with identical definitions
share one pipeline.

#### Scenario: Two materials share a pipeline
- **WHEN** two materials differ only in parameter values
- **THEN** they SHALL share one generated shader and one pipeline, differing only in their
  material data

#### Scenario: Custom surface code
- **WHEN** a material supplies a custom surface function
- **THEN** it SHALL be inlined into the generated shader at a defined extension point, with
  access to a documented set of inputs

### Requirement: Shader library and caching
Compiled shaders SHALL be stored in **shader libraries**: content-addressed artefacts containing
SPIR-V or backend-native code, reflection data, and the permutation key.

The runtime SHALL maintain a **pipeline cache** on disk keyed by device, driver version, and
engine version, populated at build time from a cooked manifest and updated at runtime.

#### Scenario: Cache reused across runs
- **WHEN** the game runs a second time on the same device and driver
- **THEN** pipelines SHALL be created from the persisted cache without recompilation

#### Scenario: Driver update invalidates the cache
- **WHEN** the driver version changes
- **THEN** the cache key SHALL differ, the old cache SHALL be discarded, and pipelines rebuilt

### Requirement: Hot reload
In development builds, editing a shader source file SHALL trigger recompilation of affected
permutations and replacement of the corresponding pipelines, without restarting.

Compilation SHALL occur on job workers; the previous pipeline SHALL remain in use until the new
one is ready. A failed compile SHALL keep the previous pipeline and report the error.

#### Scenario: Live shader iteration
- **WHEN** a developer edits a material shader and saves
- **THEN** the change SHALL appear in the viewport within a frame or two, with no restart and no
  visible stall

#### Scenario: Broken shader does not break the frame
- **WHEN** a shader edit does not compile
- **THEN** the last working pipeline SHALL continue rendering and the error SHALL be shown

### Requirement: Global shader parameters
The engine SHALL support named **global shader parameters** — project-wide values (wind, time of
day, gameplay state) settable at runtime and readable by any shader — stored in a global uniform
buffer, with no per-material update needed.

#### Scenario: One update, all shaders see it
- **WHEN** a global parameter is set
- **THEN** every shader referencing it SHALL observe the new value the next frame, with no
  material or pipeline changes

### Requirement: Compute and utility shaders
The shader system SHALL support compute shaders as first-class artefacts with the same
authoring, reflection, caching, and hot-reload path, used by culling, VFX simulation, skinning,
post-processing, and GI.

**Engine-generated shader source** — material shaders generated from material definitions, and
compute kernels generated by the VFX graph compiler — SHALL be emitted as **Slang** and SHALL pass
through this same pipeline. Generators SHALL NOT emit backend-specific source, introduce a second
shader toolchain, or maintain a separate cache.

Where available, the system SHALL support **mesh and task shaders** and **ray tracing shaders**
as capability-gated additions, with SPIR-V remaining the interchange form.

#### Scenario: Compute shader hot reload
- **WHEN** a compute shader used by a post-process pass is edited
- **THEN** it SHALL reload with the same guarantees as a graphics shader

#### Scenario: Generated kernels use one toolchain
- **WHEN** the VFX compiler produces simulation kernels
- **THEN** they SHALL be emitted as Slang and compiled, reflected, cached, and hot-reloaded by the
  existing pipeline, with no DXC or backend-specific path introduced

#### Scenario: Generated source is inspectable
- **WHEN** a generated kernel or material shader misbehaves
- **THEN** the generated Slang source SHALL be retrievable for inspection in development builds

### Requirement: Visual material editor
The editor SHALL provide a node-graph material editor that **generates a material definition**,
not an independent shader compiler. The graph SHALL be a front-end onto the same generation and
compilation path as hand-written material definitions.

The graph SHALL be able to embed custom Slang snippets in an expression node.

#### Scenario: Graph and text are interchangeable
- **WHEN** a graph-authored material is inspected
- **THEN** the editor SHALL be able to show the generated Slang source, and that source SHALL be
  what is compiled

### Requirement: Shader diagnostics
The build SHALL report: per-shader compile time, permutation counts per axis, SPIR-V instruction
counts, register and occupancy estimates where the toolchain provides them, and pipeline cache
statistics.

Development builds SHALL support GPU shader debugging through RenderDoc, PIX, and Xcode by
retaining debug information in non-shipping shader artefacts.

#### Scenario: Expensive shader is identified
- **WHEN** one material's pipeline exceeds an instruction-count threshold
- **THEN** the build report SHALL flag it with its permutation key
