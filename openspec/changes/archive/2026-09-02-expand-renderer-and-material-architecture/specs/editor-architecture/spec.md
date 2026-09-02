## MODIFIED Requirements

### Requirement: Debugging and profiling tools
The editor SHALL provide: a log panel with filtering and source navigation, a remote and local
debugger (breakpoints, stepping, variable inspection for Swift and native), a live entity
inspector for the running game, a frame profiler (CPU stages, jobs, GPU passes) with a timeline,
memory profiling by allocator tag and asset category, and a render debugger.

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
