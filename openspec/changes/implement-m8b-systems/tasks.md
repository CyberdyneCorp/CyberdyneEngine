# Tasks: M8.b — Systems

Ordered. Section 1 first and alone: it is the spike, it has a failure budget, and six of the sections
below are built on its answer.

## 1. The spike — RUN, AND IT REFUTED THE PREMISE

- [x] 1.1 Semantics stated per consumer, from each specification rather than generalised from one
- [x] 1.2 Measured against `src/rendering/material/`'s IR, the only compiled-graph implementation in
      the tree
- [x] 1.3 **Five escape hatches against a budget of two.** One IR cannot serve seven consumers: the
      material IR is a hash-consed pure-expression DAG whose identity is a content hash, so it has no
      back edges, cannot express a write, re-sorts commutative operands, deletes a value nothing
      reads, and evaluates both arms of a select. `visual-scripting`'s own "No universal
      representation" requirement forbade it by name before the spike ran
- [x] 1.4 Report committed; `design.md` §1 carries the finding

## 2. CyberGraph, the expression core, and a lowering each — `visual-scripting` → W

The spike's answer, and its three layers are separable work. **Do not modify
`src/rendering/material/`**: it is M7's closed work, one extension invalidates its cook keys, and
re-testing it mid-milestone buys nothing.

- [ ] 2.1 **CyberGraph** — the shared authoring layer every consumer adopts: nodes, typed pins, typed
      connections, stable identity, layout separated from semantics, deterministic textual source,
      subgraphs, semantic diff and three-way merge, versioning and migration, opaque preservation of
      nodes whose plugin is missing, node- and pin-precise diagnostics, the debug map, capability
      sets, the determinism audit
- [ ] 2.2 **A shared pure-expression SSA core**, generalised from the material IR by the four named
      extensions: an open type lattice, an open operation table, declared roots, and a declared phase
      boundary. Operation and type identity move to TEXT at the same time — the enumerator values are
      part of every content hash and therefore of every cook key
- [ ] 2.3 **The core's criterion is an anchor, not a port**: given the material op table and type
      lattice as a domain, it must reproduce the tree's own reference material at IR digest
      `f48f3faf395e52fd`, post-pipeline digest `178a3630921e0506` and program digest
      `7f74500626ea001a`. Hit all three and porting the material compiler later is mechanical
- [ ] 2.4 A lowering per consumer, each to the form its own specification names, **all compiling and
      none interpreting**: CyberGraph IR for scripting and abilities, an animation IR with poses as
      values and a lazily-evaluated state machine, a flat AI instruction stream over a register
      machine, and a camera rig program on the shared core
- [ ] 2.5 Execution backends, async graphs, semantic merge and debugging, per `visual-scripting`

## 3. The gameplay framework — `gameplay-framework` → W

- [ ] 3.1 Rules, session fragments, participants and teams
- [ ] 3.2 Ownership, control and authority; capabilities, tags, phases
- [ ] 3.3 Time domains, interaction, features, indexes, on M8.a's `PlaySession`

## 4. Abilities — `gameplay-abilities-and-effects` → W

- [ ] 4.1 Compiled ability programs over section 2's IR
- [ ] 4.2 Attributes and modifiers, effects and stacking, costs and cooldowns
- [ ] 4.3 Targeting and the activation pipeline

## 5. Animation — `animation-and-skinning` → W

- [ ] 5.1 Skeletons and bone LOD, clips and compression
- [ ] 5.2 The animation graph compiled to a program, batched evaluation, layers and masks
- [ ] 5.3 Root motion, IK, retargeting, the GPU pose world, pose sharing
- [ ] 5.4 `rendering-geometry-and-resources`' "Skinning" requirement, which
      `SkinningDescriptor::validate()` refuses today and whose suite asserts the refusal

## 6. AI and navigation — `ai-system` → W, `navigation` → W

- [ ] 6.1 Navmesh over Recast, runtime updates, streaming, A* and funnel, hierarchical paths
- [ ] 6.2 Flow fields, off-mesh links, avoidance, crowds
- [ ] 6.3 Agents as entities; the unified graph with tree, utility and GOAP semantics
- [ ] 6.4 Compiled behaviour programs, batched perception, knowledge, environment queries, smart
      objects, AI LOD

## 7. Cameras — `camera-system` → W

- [ ] 7.1 Compiled timelines, exact time, bindings, tracks and authority
- [ ] 7.2 Batched dispatch, arbitration, capture and restore, seek and skip, preload plans
- [ ] 7.3 Rig graphs compiled to programs, blending, framing, aim, shake, volumes, cuts

## 9. The interface — `ui-system` → W, `text-and-fonts` → C, `rendering-2d` → W

- [ ] 9.1 Dedicated element storage, the retained tree, documents, declarative authoring, layout
- [ ] 9.2 `.cyss`, data binding, input routing, the layer stack, the widget set
- [ ] 9.3 GPU-driven rendering, accessibility checks, the interface's frame budget
- [ ] 9.4 Shaping, BiDi, line breaking, layout objects, localisation
- [ ] 9.5 Sprites, ordering, batching, tilemaps, 2D lights and shadows, the screen-space SDF

## 10. Audio — `audio` → C

- [ ] 10.1 Steam Audio behind the engine's own interface, declared in `deps/manifest.toml` and
      `THIRD_PARTY.md` with a licence identifier and a justification
- [ ] 10.2 Acoustic geometry, importance tiers, voice virtualisation, effects, interactive audio

## 11. The debts this milestone inherited

- [ ] 11.1 `serialization-and-prefabs` → **C**: apply an instance's overrides back to its prefab, and
      extract a subtree into a new prefab asset. Moved here by M8.a's closing gate; unimplemented
      since M2 with a data model that has supported it since M2
- [ ] 11.2 **Assemble the renderer.** Eight modules under `src/rendering/` are linked by nothing but
      their own test binaries; nothing in the tree builds a frame out of them. `design.md` §3 states
      why this milestone is the first that cannot avoid it
- [ ] 11.3 `cy::render::MeshRenderer` has no reflected type, so an authored mesh reaches no renderer
      — the reason M8.a's artefact photograph shows an authored sphere as a box

## 12. The artefact — `samples/08-vertical-slice`

- [ ] 12.1 A level, characters that animate and think, abilities with effects, a heads-up
      interface, sound and a 2D menu. **The cinematic and the particles are M8.c's**, layered onto
      this same slice rather than a second one
- [ ] 12.2 8,000 agents and 100 concurrent effects hold their declared budgets
- [ ] 12.3 Ability activation and animation evaluation are deterministic under the simulation's
      declared profile
- [ ] 12.4 Runs from a single recipe; a recorded gap exits non-zero; the headline figure reproduces
- [ ] 12.5 Commit a screenshot

## 13. Records and gates

- [ ] 13.1 Write `tools/roadmap/milestones/m8b.toml`; declare `milestone-m8b` in `gates.toml` and
      raise `selftest.MINIMUM_CRITERIA`
- [ ] 13.2 An `m9-open` criterion using the double-star glob form
- [ ] 13.3 Update `status.yaml`, `capability-matrix.md`, `ROADMAP.md` and `dependencies.md`, and run
      the plan-consistency checks over them
- [ ] 13.4 Move `ci.yml`'s milestone job to `m8b` in the same commit that flips the gate green
- [ ] 13.5 Open the M9 change

## 14. The gate

- [ ] 14.1 Full audit, per `delivery-roadmap`
- [ ] 14.2 Clean build of every profile from empty; `test-all` in each
- [ ] 14.3 Every gate by hand; the M8.b ledger run once
- [ ] 14.4 Every criterion actually executes something
- [ ] 14.5 Adversarial pass: find a per-entity virtual tick in a graph consumer; write gameplay state
      from VFX and from inference and confirm both are refused; exceed each declared budget and
      confirm the cost is bounded rather than the frame
- [ ] 14.6 Records verified against what the code supports, not what the plan claimed — and the
      spike's failure budget reconciled against what it actually spent
