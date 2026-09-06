# Tasks: M5 — Authorable

Ordered. The ledger flattening and the live-bridge spike run first and are independent of each
other. Everything after depends on both.

## 0. Two things before any editor code

### 0.1 The flattened milestone ledger — **first, and it pays for itself immediately**

A ledger currently begins by running the previous milestone's, which runs the one before it. By M5
that is five deep and takes the better part of a day. M4's gate found a unit case that failed
**four ledgers at once** purely through nesting, and its own advice was "one ledger at a time, and a
modest CY_JOBS" — advice that stops being necessary once the nesting is gone.

- [ ] 0.1.1 A ledger evaluates the permanent gate set **once, deduplicated**, plus its own new
      criteria, and does not invoke another milestone's ledger
- [ ] 0.1.2 Remove the `m<n>-green` chaining criteria from m1 through m4
- [ ] 0.1.3 Prove the deduplication: `four-profiles` executes **once** in a full run of the newest
      ledger, against four times today
- [ ] 0.1.4 Confirm the ladder is still enforced — a regression in an M0 criterion must still fail
      the newest ledger, because that criterion is in the permanent set
- [ ] 0.1.5 Record before and after: invocation count, distinct criteria, wall-clock

### 0.2 Spike — live bridge latency

M5's named risk. Its only deliverable is a decision.

- [ ] 0.2.1 Measure gizmo-drag round-trip latency over the out-of-process boundary, before any panel
      is built
- [ ] 0.2.2 Measure it again for a runtime that is not local
- [ ] 0.2.3 If interactive manipulation cannot feel local, propose the change before section 2. The
      honest alternative — in-process hosting as the local default, out-of-process as the remote
      path — is already permitted by `editor-rust-application`, so this is a default change rather
      than an architecture change. Say so if that is the answer.

## 1. Carried forward from M4

- [ ] 1.1 **Reload while running is unimplemented, and it is M5's whole value proposition.** M4's
      spike proved the model — serialize, migrate-by-name, recreate, never `dlclose` — but nothing
      reloads a module while the runtime is live. This is the first thing the editor needs.
- [ ] 1.2 **The ABI table has 31 entries and no chunk, node, `CyStage` or `CySeverity` entry**, and
      two Swift enums are hand-copied from engine enums with nothing to check them against. One was
      **already wrong** — six enumerators against three — so every `Log.info` reached the engine as
      an error on a green run. Generate the enums from the ABI description like everything else, and
      add the appends the editor and the Rust SDK need.
- [ ] 1.3 **Nothing is reflected, so the editor's inspector has nothing to read.** The generated
      inspector is a headline M5 requirement and it needs engine types carrying
      `reflect::TypeInfo`. This is also M2's state-hash gap, still open and widening.
- [ ] 1.4 Swift on all six CI legs before it can be a hard requirement — two are ARM, which no Swift
      setup action covers today. Decide and record whether M5 makes it hard or keeps it optional.
- [ ] 1.5 `cmake/features.cmake` — audit every milestone annotation against the capability matrix.
      `CY_VIRTUAL_GEOMETRY` says M10 where the roadmap says M7; `CY_UI` says M9 against M8.

## 2. The editor as a Rust client — `editor-rust-application` → Working

- [ ] 2.1 The Rust workspace, and the profile mapping finally exercised — M0's spike wrote the Cargo
      column and reserved it unused for five milestones
- [ ] 2.2 Engine hosting modes; safety and interoperation rules
- [ ] 2.3 The **editor SDK generated against the ABI**, as the Swift overlay is
- [ ] 2.4 MVVM: view models are not a second source of truth; no peer dependencies between panels
- [ ] 2.5 **Commands are the single action surface**, with metadata sufficient for machine
      invocation — typed parameters, a description written for a caller that cannot see the
      interface, and a declared effect class. Binds from the **first command**, per
      `editor-agent-interface`; retrofitting it across an established registry is an entry-by-entry
      migration.
- [ ] 2.6 Asynchronous operations; change propagation; editor state model
- [ ] 2.7 Editor testability; the toolkit stays an implementation detail

## 3. Documents and transactions — `editor-documents-and-transactions` → Working

- [ ] 3.1 Authoring, preview and runtime worlds kept distinct
- [ ] 3.2 Documents as the unit of editing; workspace and view state
- [ ] 3.3 **Transactions are the only path for persistent mutation** — the invariant. Undo, autosave,
      crash recovery, semantic diff, merge and live editing are one mechanism read six ways, and
      that is only true if nothing writes around it.
- [ ] 3.4 Operations address stable identities; deltas rather than snapshots
- [ ] 3.5 Interactive, nested and coalesced transactions; history scope and memory
- [ ] 3.6 The journal, autosave and recovery
- [ ] 3.7 Semantic diff and merge; selection and property binding
- [ ] 3.8 **Transactions carry provenance** — the actor, and for an agent the session and intent
- [ ] 3.9 The test that writes around the transaction system, and fails

## 4. Viewport, gizmos and the interface

- [ ] 4.1 Viewport transport; multiple viewports and view states; navigation
- [ ] 4.2 **Engine-side picking** — what is picked is what was rendered, including instanced and
      skinned content
- [ ] 4.3 Gizmos and manipulation over the engine's own path; snapping and precision
- [ ] 4.4 View modes and debug visualisation; overlays; editing while playing
- [ ] 4.5 Viewport performance and degradation; determinism and reproduction
- [ ] 4.6 `editor-ui-ux` → Working: docking and workspaces, density, command palette, keyboard-first
      operation, the **generated inspector**, validation surfacing, notifications
- [ ] 4.7 `editor-visual-language` → Seed: the semantic palette, the axis language, gizmo legibility,
      the orientation widget that is not a manipulator, chrome as overlay

## 5. Import, live editing and text

- [ ] 5.1 `asset-import-pipeline` → Working: the importer framework, glTF and texture import over
      meshoptimizer, the cook cache, dependency tracking
- [ ] 5.2 `live-editing` → Working: live edit as a compilation step, policies, asset and shader
      reload, play modes, the live bridge, runtime inspection — over M4's proven reload model
- [ ] 5.3 `text-and-fonts` → Seed: `TextServer` over HarfBuzz, ICU and FreeType; glyph atlases for
      viewport and overlay text
- [ ] 5.4 `editor-agent-interface` → Seed: the projection of the command registry, resources for the
      scene and selection, viewport observation, scope and effect class

## 6. The artefact

- [ ] 6.1 `samples/05-editor-session` — a scripted session: open a project, import a glTF asset,
      manipulate it with gizmos, undo, save, enter play mode
- [ ] 6.2 **Kill the hosted runtime mid-session; the editor survives with the document intact**
- [ ] 6.3 Editor headless tests in CI

## 7. Closing the milestone

- [ ] 7.1 Killing the runtime leaves the editor running with an unsaved document intact
- [ ] 7.2 Every persistent mutation in the session went through a transaction, proven by an audit hook
- [ ] 7.3 Undo/redo round-trips the full session; the journal replays after a simulated crash
- [ ] 7.4 Picking returns the object the renderer actually drew
- [ ] 7.5 The generated inspector edits a reflected type with no per-type editor code
- [ ] 7.6 A Swift module reloads while the runtime is live, preserving world state
- [ ] 7.7 All four profiles build clean and `just test-all` is green in each
- [ ] 7.8 Sanitizers green over the new suites
- [ ] 7.9 **`just roadmap-milestone m5` exits zero — and evaluates the permanent set once rather
      than nesting four ledgers**
- [ ] 7.10 Update `status.yaml` and `capability-matrix.md`; record what is thinner than the tasks claim
- [ ] 7.11 `openspec validate --specs --strict` passes; archive this change
- [ ] 7.12 Open the M6 change — the derivation key model is its named spike
