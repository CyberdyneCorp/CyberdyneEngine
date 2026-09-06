# Implement M5 — Authorable: the editor as a client, and a ledger that stops compounding

## Why

M4 made a game you can write. M5 makes one you can *edit*, and it does so from a separate process in
a different language — which is the decision that has to be paid for once, here, rather than
discovered later.

**The editor is a client, not part of the engine.** A Rust application talking to a hosted runtime
over the C ABI and the live bridge, so a runtime crash costs a restart rather than a session, the
boundary is enforced by the language rather than by discipline, and editing on a remote or embedded
target is the same code path as editing locally. Every one of those follows from being out of
process, and none of them can be retrofitted onto an in-process editor.

**Transactions are the only path for persistent mutation**, and this is the invariant the roadmap
pins to M5. Undo, autosave, crash recovery, semantic diff, three-way merge and live editing are not
six features — they are one mechanism read six ways, and that is only true if nothing writes around
it. A single direct write path does not merely bypass undo; it makes the other five silently
incomplete for that property, and nothing will point at it.

The third reason is arithmetic. **The milestone ledger's cost is compounding and M5 is the cheapest
place to stop it.** Each milestone's ledger currently begins by running the previous one's, which
runs the one before it — twelve deep by M11. Measured across the first five: 123 criterion
invocations over 89 distinct criteria, and `four-profiles` executed four times by a single run.
That multiplication has already manufactured a failure in this project, and it grows with every
milestone we do not fix it in.

## What Changes

- **The editor as a Rust application.** The workspace, hosting modes, safety and interoperation
  rules, MVVM with services and commands, and the editor SDK generated against the ABI so the two
  cannot drift — the same mechanism that produces the Swift overlay.
- **Documents and transactions.** Authoring, preview and runtime worlds kept distinct; transactions
  as the only write path; operations addressing stable identities; deltas rather than snapshots;
  interactive, nested and coalesced transactions; the journal, autosave and crash recovery; semantic
  diff and merge.
- **Viewport and gizmos.** Viewport transport, engine-side picking so what is picked is what was
  drawn, gizmos and manipulation, snapping, view modes, and degradation — with the rendering
  responsibility split held: the editor decides what should be shown, the renderer decides how it is
  drawn, and there is no second renderer.
- **Asset import.** The importer framework, glTF and texture import, the cook cache, dependency
  tracking.
- **Live editing.** Live edit as a compilation step, policies, asset and shader reload, play modes,
  the live bridge, runtime inspection — over the hot-reload model M4's spike proved.
- **Command metadata for machine invocation**, from `editor-agent-interface`: typed parameters, a
  description written for a caller that cannot see the interface, and a declared effect class. This
  binds from the **first command registered**, because retrofitting it across an established registry
  is an entry-by-entry migration.
- **The flattened ledger**, below.

**Closing artefact**: `samples/05-editor-session` — a scripted session that opens a project, imports
a glTF asset, manipulates it with gizmos, undoes, saves, enters play mode, and recovers with the
document intact after the hosted runtime is killed mid-session.

## Capabilities

### Advanced Capabilities

`editor-rust-application`, `editor-documents-and-transactions`, `editor-architecture`,
`editor-viewport-and-gizmos`, `editor-ui-ux`, `asset-import-pipeline` and `live-editing` to
**Working**; `text-and-fonts` and `editor-agent-interface` to **Seed**; `project-and-plugins`,
`core-type-system`, `scene-graph-and-nodes` and `native-abi` to **Complete**.

### Modified Capabilities

- `delivery-roadmap` — **the permanent gate set is flat and deduplicated, and a ledger does not
  re-run an earlier ledger.** Chaining is the obvious implementation and it compounds: 34 of 123
  invocations across five milestones are redundant, and re-running one criterion many times
  multiplies its failure probability by the same factor. A marginal unit test already became roughly
  a dozen exposures per pull request through nesting alone, and cost a diagnosis cycle before the
  multiplication was recognised as the cause. Deduplication is a correctness property here, not an
  optimisation.

## Impact

- **New code**: `editor/` — the first Rust in the repository, and a second application rather than a
  larger one. Plus `src/servers/text/`, the importer under `tools/`, and
  `samples/05-editor-session/`.
- **New dependencies**: the editor's Rust crates, HarfBuzz, ICU and FreeType behind `TextServer`,
  and glTF plus meshoptimizer behind the importer framework — each already named in
  `thirdparty-dependencies`' intended set.
- **Toolchain**: Cargo 1.92 is installed. The four build profiles must mean the same thing in Cargo
  that they mean in CMake — the mapping was written down at M0's spike and reserved unused since,
  and M5 is where it is finally exercised.
- **Carried forward from M4**: the gate's findings, plus the stale milestone annotations in
  `cmake/features.cmake` — `CY_VIRTUAL_GEOMETRY` says M10 and `CY_UI` says M9, where the roadmap
  places them at M7 and M8. Harmless while the options are off, and exactly the drift that misleads
  someone later.
- **Risk**: the live bridge. An out-of-process editor is the right decision and the expensive one —
  every selection, gizmo drag and property edit crosses a process boundary, and whether that feels
  local is measured rather than argued.
