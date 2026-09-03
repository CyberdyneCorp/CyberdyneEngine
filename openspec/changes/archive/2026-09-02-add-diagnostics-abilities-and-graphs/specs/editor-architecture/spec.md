## MODIFIED Requirements

### Requirement: Debugging and profiling tools
The editor SHALL provide: a log panel with filtering and source navigation, a remote and local
debugger (breakpoints, stepping, variable inspection for Swift and native), a live entity
inspector for the running game, a frame profiler (CPU stages, jobs, GPU passes) with a timeline,
memory profiling by allocator tag and asset category, and a render debugger.

**The editor is a client of the diagnostics backend defined in `diagnostics-profiling-and-crash`,
not its owner.** Trace transport, buffering, capture artefacts, crash artefacts, and reproduction
belong to that capability, and the same data SHALL be consumable by command-line tools, automated
systems, and a remote session with no editor attached.

The editor SHALL be able to open capture, crash, and reproduction artefacts **without the game
running**, including artefacts produced on another platform.

The **render debugger** SHALL show the render graph as it was built for a frame: passes and their
dependencies as a navigable graph, and per pass its GPU time, queue, resources read and written,
transient memory, and barrier wait time. It SHALL show the budget arbiter's allocations,
measurements, and adjustments for that frame.

A **shader and material inspector** SHALL show, for any material, each stage of lowering — graph,
material IR before and after optimisation, generated Slang, backend binary — with instruction
counts, occupancy estimates, resource bindings, cost attributed to graph nodes, and the reason
each permutation exists.

Profiler markers SHALL map cleanly into external GPU capture tools (RenderDoc, PIX, Xcode, Nsight,
Radeon GPU Profiler), and a capture SHALL be launchable from the editor.

These tools SHALL be built alongside the renderer rather than after it.

Live editing SHALL be supported: property changes in the editor applied to the running game.

#### Scenario: Frame spike investigation
- **WHEN** a frame exceeds its budget
- **THEN** the profiler timeline SHALL show which stage, system, or GPU pass consumed the time

#### Scenario: Live tweak
- **WHEN** a value is changed in the inspector while the game runs
- **THEN** it SHALL apply immediately in the running game, with a clear indication that the change
  is not persisted unless applied

#### Scenario: Quality drop is explained
- **WHEN** rendering quality drops during play
- **THEN** the render debugger SHALL show which allocation changed and what measurement caused it

#### Scenario: Expensive material is diagnosed
- **WHEN** a material is expensive
- **THEN** the inspector SHALL attribute the cost to specific graph nodes rather than reporting a
  total

#### Scenario: An artefact opens without the game
- **WHEN** a capture or crash artefact arrives from a tester on another platform
- **THEN** the editor SHALL open it without that runtime present

### Requirement: Specialised editors
The editor SHALL provide dedicated editors for: materials (including the node graph),
animation graphs and clips (a timeline with curve editing), the VFX graph (see `vfx-system`),
**terrain**, **foliage**, **water**, and **environment fields**, tilemaps, UI layout, audio buses
and mixing, navigation baking, lighting and lightmap baking, **abilities and effects**, **gameplay
and utility graphs**, and localisation tables.

**All node-graph editors SHALL be built on the shared graph infrastructure defined in
`visual-scripting`**: one canvas, one identity model, one serialization and diff format, one
debugging model — while each domain keeps its own lowering. A sixth bespoke graph editor SHALL NOT
be created.

The environment tools SHALL include: terrain sculpting and material painting over a
**non-destructive modifier stack**, biome and field painting, river spline authoring with live flow
and shoreline preview, lake and ocean configuration, foliage painting and rule authoring with
regional preview, and road and spline tools.

Rule-driven tools — foliage placement, field-driven terrain materials, procedural generation — SHALL
be able to show **why** a result occurred, naming the input that drove or excluded it, since a
procedural result that cannot be explained cannot be corrected.

Each SHALL be a plugin using the same panel and undo infrastructure as user plugins, so the
extension API is exercised by the engine's own tooling.

#### Scenario: The engine dogfoods its plugin API
- **WHEN** a built-in editor is implemented
- **THEN** it SHALL use only the public plugin API, so any limitation is discovered internally
  first

#### Scenario: Graph editors share infrastructure
- **WHEN** the material graph, the VFX graph, and a gameplay graph editor are implemented
- **THEN** they SHALL share the node-graph canvas, identity, serialization, diffing, undo, and
  debugging infrastructure rather than each implementing its own

#### Scenario: A procedural result is explainable
- **WHEN** a designer asks why no trees appear in a region
- **THEN** the tool SHALL name the rule input responsible

#### Scenario: Sculpting is non-destructive
- **WHEN** a designer inserts an erosion pass beneath existing sculpting
- **THEN** the sculpting SHALL be preserved and reapplied above it
