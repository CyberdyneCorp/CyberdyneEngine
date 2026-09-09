# Tasks: M8.b — Systems

Ordered. Section 1 first and alone: it is the spike, it has a failure budget, and six of the sections
below are built on its answer.

## 1. The spike — one graph IR against seven consumers

- [ ] 1.1 State, per consumer, the semantics its specification requires of an authored graph:
      `visual-scripting`, `gameplay-abilities-and-effects`, `ai-system`, `animation-and-skinning`,
      `sequencing-and-cinematics`, `vfx-system`, `camera-system`
- [ ] 1.2 Measure each against `src/rendering/material/`'s IR — the only compiled-graph
      implementation in the tree — and record what it expresses, what it could express with a
      specified extension, and what it cannot
- [ ] 1.3 Spend against `design.md` §1's failure budget explicitly: name every extension and every
      escape hatch. **Two escape hatches ends the milestone's premise and re-plans it**
- [ ] 1.4 Commit the spike's report, as M7's two are committed

## 2. The shared graph — `visual-scripting` → W

- [ ] 2.1 Shared graph infrastructure, typed pins, stable node identity
- [ ] 2.2 The IR, and execution backends
- [ ] 2.3 Async graphs, semantic merge, debugging
- [ ] 2.4 **No graph is interpreted at runtime** — a test that fails on a per-entity virtual tick in
      any consumer, not a review that looks for one

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

## 7. Sequences and cameras — `sequencing-and-cinematics` → W, `camera-system` → W

- [ ] 7.1 Compiled timelines, exact time, bindings, tracks and authority
- [ ] 7.2 Batched dispatch, arbitration, capture and restore, seek and skip, preload plans
- [ ] 7.3 Rig graphs compiled to programs, blending, framing, aim, shake, volumes, cuts

## 8. Effects — `vfx-system` → W

- [ ] 8.1 The graph compiler and IR, GPU-first simulation, derived attribute layout
- [ ] 8.2 The unified world and scheduler, data interfaces, GPU scene integration, GPU events
- [ ] 8.3 Budget scalability, and **the determinism firewall**: VFX cannot write gameplay state,
      proven by a test rather than by a convention

## 9. The interface — `ui-system` → W, `text-and-fonts` → C, `rendering-2d` → W

- [ ] 9.1 Dedicated element storage, the retained tree, documents, declarative authoring, layout
- [ ] 9.2 `.cyss`, data binding, input routing, the layer stack, the widget set
- [ ] 9.3 GPU-driven rendering, accessibility checks, the interface's frame budget
- [ ] 9.4 Shaping, BiDi, line breaking, layout objects, localisation
- [ ] 9.5 Sprites, ordering, batching, tilemaps, 2D lights and shadows, the screen-space SDF

## 10. Audio and inference — `audio` → C, `ml-inference` → S

- [ ] 10.1 Steam Audio, acoustic geometry, importance tiers, voice virtualisation, effects,
      interactive audio
- [ ] 10.2 Model assets, tensors and sessions, the backend abstraction, **the determinism boundary**

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

- [ ] 12.1 A level, characters that animate and think, abilities with effects, a cinematic, a
      heads-up interface, effects, sound and a 2D menu
- [ ] 12.2 8,000 agents and 100 concurrent effects hold their declared budgets
- [ ] 12.3 Ability activation, sequence playback and animation evaluation are deterministic under
      the simulation's declared profile
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
