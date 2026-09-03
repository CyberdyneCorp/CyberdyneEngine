# Tasks: diagnostics, abilities, and graphs

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change is archived on that basis. Sections 3 onward are the implementation backlog, sequenced by the
phase table in `design.md`.

## 1. Specification

- [x] 1.1 Record in `design.md`: thirty-nine diagnostics requirements with no shared transport,
      telemetry that must not perturb what it measures, the profiler that cannot be started after the
      problem, crash artefacts that survive the absence of everything, abilities as the module the
      framework promised, the two details that decide whether an ability system is usable, prediction
      needing identity, graphs as shared infrastructure with separate languages, graphs as an
      authoring language, what a graph compiler can enforce that review cannot, diff and merge as the
      place visual scripting usually fails, and the phase table
- [x] 1.2 New `diagnostics-profiling-and-crash` (18 requirements): one trace with many producers,
      compiled identity, buffering and loss policy, profiler views, structured logging, assertions
      and health, the rolling buffer and automatic capture, crash artefacts, breadcrumbs, graphics
      device diagnostics, shader lineage, reproduction artefacts, privacy classification, remote and
      server diagnostics, capture artefacts, telemetry export, overhead, and forbidden patterns
- [x] 1.3 New `gameplay-abilities-and-effects` (20 requirements): the optional module, compiled
      programs, state and sets, attributes, modifiers with specified order, effects, stacking
      policy, costs and cooldowns as ticks, targeting, the activation pipeline with structured
      validation, activation identity and prediction, deterministic execution, asynchronous
      behaviour, cues, bulk activation, artificial intelligence integration, persistence and replay,
      diagnostics, performance, and forbidden patterns
- [x] 1.4 New `visual-scripting` (20 requirements): shared infrastructure with domain lowering,
      graphs as an authoring language, typed pins, stable identity, events rather than tick, the
      intermediate representation, two backends, function metadata, asynchronous graphs, determinism
      auditing, capabilities, semantic diff and merge, hot reload, node versioning and missing nodes,
      debugging, compiler-grade diagnostics, testing, interoperability, performance, and forbidden
      patterns
- [x] 1.5 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 **Two promises kept.** `gameplay-framework` said abilities would be an optional module a
      game without them does not pay for, and that gameplay visual scripting was deferred with a
      compile-to-system seam preserved. Both are now filled on exactly those terms.
- [x] 2.2 `editor-architecture` — the editor becomes a **client** of the diagnostics backend rather
      than its owner, and can open capture, crash and reproduction artefacts without the game
      running; all node-graph editors are required to share the graph infrastructure so a sixth
      bespoke editor is not created
- [x] 2.3 `core-jobs-and-concurrency` and `core-memory-and-containers` — their diagnostics emit into
      the shared trace, so a task stall, a memory spike and a streaming stall appear on one timeline;
      allocation call-stack capture becomes a declared mode
- [x] 2.4 `rhi-and-render-graph` — breadcrumbs use the shared mechanism so they survive into crash
      artefacts, and device loss contributes graph, pipeline and resource state rather than a driver
      message alone
- [x] 2.5 `thirdparty-dependencies` — diagnostics infrastructure, the ability module and the graph
      infrastructure recorded as engine-built, with platform crash handling, symbol formats,
      compression and capture tooling integrated beneath
- [x] 2.6 **The honest boundary on graphs.** Materials, VFX, animation, AI and camera rigs keep their
      own intermediate representations; what is shared is nodes, pins, identity, serialization,
      diffing and debugging. A universal intermediate representation is a recorded non-goal, because
      a material's algebra and a gameplay event's control flow are not one language.
- [x] 2.7 `replay-and-rollback`, `simulation-and-determinism`, `gameplay-framework`,
      `networking-and-replication`, `ai-system` — reviewed; no change needed. The crash replay buffer,
      the determinism rules and firewall, the command stream and validation path, prediction and
      rollback, and the requirement that agents act through commands already provide what these
      capabilities consume.
- [x] 2.8 **Non-goals recorded**: one universal graph representation, graphs as the runtime object
      model, sandboxing native code through graph capabilities, and an ability system every game must
      use

## 3. Diagnostics (deferred, and first)

- [ ] 3.1 Trace schema, compiled identifiers, metadata table
- [ ] 3.2 Per-thread buffers, channel priorities, loss policy and reporting
- [ ] 3.3 Task, ECS, memory, GPU, IO, residency, input and simulation views
- [ ] 3.4 Structured logging with typed fields and categories
- [ ] 3.5 Assertion levels and the health model
- [ ] 3.6 Rolling buffer, automatic capture triggers, capture artefact format and viewer
- [ ] 3.7 Crash artefacts, breadcrumbs, defensive capture path
- [ ] 3.8 Device-loss capture with graph, pipeline and resource state
- [ ] 3.9 Symbolication against archived symbols; privacy classification and redaction
- [ ] 3.10 Remote transport; dedicated-server diagnostics
- [ ] 3.11 Reproduction artefacts linked to the crash replay buffer
- [ ] 3.12 Telemetry export, opt-in and declared

## 4. Abilities (deferred)

- [ ] 4.1 Attribute definitions, ECS-native storage, compiled bindings
- [ ] 4.2 Modifier operations with the specified evaluation order and stable tie-breaks
- [ ] 4.3 Ability definitions and the compiler; ability sets and grants
- [ ] 4.4 Compact ability and effect state; effect scheduling on ticks
- [ ] 4.5 Stacking policies
- [ ] 4.6 Costs with transactional validate, reserve and commit; cooldowns and charges
- [ ] 4.7 Target data, acquisition policies and compiled validation
- [ ] 4.8 Activation pipeline with structured validation results
- [ ] 4.9 Activation identity; prediction policies and reconciliation
- [ ] 4.10 Deterministic execution, derived streams, snapshot participation
- [ ] 4.11 Asynchronous abilities as compiled state machines; cancellation
- [ ] 4.12 Cues with simulation points and speculative or confirmed classification
- [ ] 4.13 Bulk activation and area effect application
- [ ] 4.14 Planning metadata and agent integration
- [ ] 4.15 Ability debugger and activation timeline

## 5. Graph infrastructure (deferred)

- [ ] 5.1 Node and pin model, typed connections, stable identity, layout separated from semantics
- [ ] 5.2 Deterministic textual source; semantic diff and three-way merge
- [ ] 5.3 Editor canvas, undo, subgraphs, palette — adopted by existing domain editors
- [ ] 5.4 Node versioning, migration, and missing-node preservation
- [ ] 5.5 Graph intermediate representation and compiler passes
- [ ] 5.6 Access analysis producing scheduler declarations
- [ ] 5.7 Bytecode backend: typed register machine, shared program, separate state
- [ ] 5.8 Native backend and cross-backend equivalence verification
- [ ] 5.9 Gameplay graph frontend lowering to systems
- [ ] 5.10 Ability graph frontend lowering to ability programs
- [ ] 5.11 Asynchronous lowering with compact state and weak references
- [ ] 5.12 Determinism auditing and capability enforcement
- [ ] 5.13 Debug map, breakpoints, value inspection, prediction comparison
- [ ] 5.14 Graph tests runnable headlessly; editor mocked execution

## 6. Validation (deferred)

- [ ] 6.1 Diagnostics overhead measured against the declared bounds, in shipping and development
- [ ] 6.2 Emission allocates nothing and takes no global lock; loss under pressure is reported
- [ ] 6.3 Crash capture works with no editor, no debugger, no network and no symbols present
- [ ] 6.4 Device-loss capture names the responsible pass on each backend
- [ ] 6.5 A reported defect produces a reproduction artefact that replays the window
- [ ] 6.6 Privacy: no classified field leaves the machine in an uploaded artefact
- [ ] 6.7 Ability scale: a hundred thousand owners, ten thousand activations per second, no
      per-activation allocation
- [ ] 6.8 Modifier and effect ordering identical across machines, worker counts and chaos scheduling
- [ ] 6.9 Prediction: a rejected predicted activation reverts cleanly with no duplicated cues
- [ ] 6.10 Ability state round-trips through save, rollback and replay
- [ ] 6.11 Graph scale: a hundred thousand behaviour-bearing entities with no per-entity instance
- [ ] 6.12 Backend equivalence: bytecode and native produce identical results
- [ ] 6.13 Determinism audit rejects a graph reaching non-deterministic functionality, directly and
      indirectly
- [ ] 6.14 Graph merge: independent edits merge, same-element edits conflict, layout changes do not
- [ ] 6.15 A graph with missing plugin nodes opens, preserves them, and restores on re-enable

---

**Archived 2026-09-02.** Sections 1 and 2 are complete: `diagnostics-profiling-and-crash`,
`gameplay-abilities-and-effects` and `visual-scripting` are in `openspec/specs/`, and six
capabilities were updated. **Two promises the gameplay framework made are kept** — abilities as an
optional module a game without them does not pay for, and gameplay visual scripting compiling to
systems rather than being interpreted per entity. The unchecked items from section 3 onward are the
implementation backlog; **diagnostics comes first**, because everything after it is easier to build
with it than without it — including the other two capabilities in this same change.
