# Tasks: editor agent interface

Specification-stage change. Sections 1 and 2 are the work of this change.

Section 3 records the implementation, **deferred to M5 and M8** — none of it can be done before
there is an editor and a command registry to project. It is listed so the scope is not lost.

## 1. Specification

- [x] 1.1 Record in `design.md`: why the projection makes this small, why observation matters more
      than mutation, what attribution is and is not, why confirmation is rare, reproducibility, and
      the rejected alternatives
- [x] 1.2 New `editor-agent-interface` capability: agents as clients, MCP behind an engine-owned
      interface, tools projected from the command registry, resources as the read surface, viewport
      observation, engine-side picking, the manipulation path, transactions, attribution,
      explainable refusal, non-exclusivity, scope and effect class, confirmation, reproducibility,
      budget, generated discovery, trace integration, and forbidden patterns
- [x] 1.3 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 `editor-rust-application` — "Commands are the single action surface" names the agent
      interface concretely and requires metadata sufficient for machine invocation, binding from
      the first command
- [x] 2.2 `editor-documents-and-transactions` — transactions carry provenance, with the explicit
      statement that attribution is not a security control
- [x] 2.3 `editor-viewport-and-gizmos` — reviewed; no change needed. Engine-side picking and the
      capture rules already say what the agent interface needs; it consumes them rather than
      extending them
- [x] 2.4 `editor-ui-ux` — reviewed; no change needed. "An action reachable only from one widget
      with no command registration" is already forbidden, which is what makes the projection complete
- [x] 2.5 `editor-visual-language` — reviewed; no change needed. Tool descriptions inherit the
      vocabulary requirement, including the search-alias mechanism for another engine's terms
- [x] 2.6 `project-and-plugins` — reviewed; no change needed. Trust tiers govern extensions; the
      agent interface's scope declaration is its own mechanism and does not redefine them
- [x] 2.7 `diagnostics-profiling-and-crash` — reviewed; no change needed. Agent activity is trace
      data with a privacy classification like any other

## 3. Implementation (deferred to M5 and M8)

### 3.1 M5 — Seed, with the editor

- [ ] 3.1.1 Command metadata: typed parameters, machine-readable descriptions, effect class
- [ ] 3.1.2 The projection: tools generated from the registry, with declared exclusions carrying reasons
- [ ] 3.1.3 Transactions carry actor attribution; the undo stack and journal display it
- [ ] 3.1.4 The MCP transport behind an engine-owned interface, gated at build time
- [ ] 3.1.5 Resources for the scene hierarchy, an entity's properties, and the selection
- [ ] 3.1.6 Viewport observation returning the engine-rendered image, with overlay state declared
- [ ] 3.1.7 Scope declaration and enforcement; confirmation for irreversible effect classes
- [ ] 3.1.8 The connected-agent indicator, with pause and revoke

### 3.2 M8 — Working, when there is a project worth driving

- [ ] 3.2.1 Manipulation through the interactive path, with pivot, space, snapping and constraints
- [ ] 3.2.2 Engine-side picking and spatial queries
- [ ] 3.2.3 Debug and buffer visualisations as observable views
- [ ] 3.2.4 Resources for assets, diagnostics, documents, and the play and build state
- [ ] 3.2.5 Grouped transactions, so a multi-step agent edit undoes as one
- [ ] 3.2.6 Session recording, export, and replay against a recorded revision
- [ ] 3.2.7 Budget: invocation rate, render cost, concurrency, all reported to the agent
- [ ] 3.2.8 Trace integration with cost attribution per agent and operation
- [ ] 3.2.9 A conformance suite proving no agent capability exceeds a human one
