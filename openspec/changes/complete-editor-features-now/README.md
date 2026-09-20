# complete-editor-features-now

Complete the currently implementable CyberEditor authoring features without depending on unfinished renderer, remote-device, or build-service backends.

## Scope ledger

This ledger is the guardrail for “everything that can be done now.” A feature may leave this change
only when the named producer is absent or another active change owns the same files and deliverable.

| Roadmap work | This change delivers now | Dependency / owner kept open |
|---|---|---|
| M11.b 2.5 plugin dogfooding | Panels, commands, and views remain behind current public Editor abstractions | Binary C ABI descriptor and authoritative project graph: M11.b 2.2/2.4 and `project-and-plugins` |
| M11.b 3.3 specialised editors | Shared graph/timeline surfaces and existing material/gameplay/ability/animation/sequence hosts are preserved and exposed where their vocabularies work | Remaining domain vocabularies, canonical writers, previews, and lowerings: their owning capability rows; renderer-dependent material work: `implement-m11c-image` |
| M11.b 3.5 rule explanations | No false explanation UI | PCG, foliage, terrain, water, and field provenance producers do not yet expose the required records |
| M11.b 3.6 projects/settings | Settings panel, templates, categories, search, platform overrides, preferences | Engine/Swift package dependency graph: M11.b 2.4 and `project-and-plugins` |
| M11.b 3.7 build/deploy | Swift build/reload UI over the existing project service | General compile/cook/package/deploy service and device installation: `implement-m11d-desktop` |
| M11.b 3.8 debugging/profiling | Editor diagnostics, build diagnostics, agent activity, source navigation; UI for data already exposed | Native/Swift stepping transport, GPU/render/material records and capture launch require diagnostics/render/build producers |
| M11.b 3.9 offline artefacts | Add viewers only for artefact schemas currently readable through stable APIs | Cross-platform capture/crash/reproduction readers are not exposed to the Rust SDK yet |
| M11.b 3.10 live reload | Swift build/reload workflow and visible results over existing services | Asset/shader/material publication and full keep-changes data require runtime producers |
| M11.b 4.3 document completion | Semantic diff, three-way merge, conflict resolution, document tabs/history | Domain operations without a canonical format remain explicit refusals |
| M11.b 5.1 view modes | Existing nineteen modes remain command/MCP accessible | Eight engine views in `PLANNED_VIEWS`; five are renderer-owned by M11.c |
| M11.b 5.2 remote transport | No placeholder remote control | `EncodedStream` producer and remote-device session transport |
| M11.b 5.4 remaining Editor rows | MCP desktop coexistence, documents, hierarchy, assets, settings, source control, history, Swift Workspace | Engine-produced images/spatial data remain with their producers |
| M11.b 5.5 crash survival | Preserve and extend existing local hosted-runtime coverage | Remote-device runtime is unavailable, so three-mode proof cannot yet be honest |
| M11.b 6.x authoring formats | Import-settings UI for formats the importer actually supports | Missing codec/compressor/importer dependencies remain in M11.b section 1/6 |
| M11.b 8.x graph consumers | No bespoke or fake domain editor; use the shared surfaces | Lowerings, execution backends, source formats, and domain runtimes named in section 8 |
| M11.b 9.3–9.5 Swift | Embedded source workflow, SourceKit-LSP, current dynamic build/reload | Shipping static configuration/toolchain matrix and missing ABI/lifecycle callbacks |

## Local product gaps included beyond the milestone checklist

- Visible multi-document tabs, safe dirty close, and workspace restoration.
- Source Control, Settings, and attributed Undo History panels over existing services.
- Hierarchy multi-selection, rename, reparenting, and template creation.
- Asset folder/filter/move/rename/drop/import-settings workflows that need no renderer.
- Concurrent, visible, pausable, revocable desktop MCP sessions.
- An embedded Swift Workspace that remains an ordinary Swift package.

See `baseline.md` for the reproducible starting inventory and `tasks.md` for the completion proof.

## Accessibility and layout evidence

Task 10.3 is exercised by `cy-editor-shell/tests/new_panels_are_accessible.rs`. The headless egui
harness renders Undo History, Settings, Source Control, Agent Sessions, Swift Workspace, Semantic
Diff, and Semantic Merge through the production `Panels` adapter in all 56 combinations of dark or
light theme, compact or comfortable density, and 900x600 or narrow 280x360 panel bounds. Every case
must paint, retain its distinguishing AccessKit label/value, and complete without layout failure.
The same harness sends keyboard Tab input with no pointer events and requires every enabled action
or screen-reader-readable empty state to receive focus. Empty-state labels use explicit
non-interactive focus semantics so an unavailable or empty panel is not silent to a screen reader.

No screenshots are attached: the available regression path is `egui::Context` running without a
window, display, GPU, or raster renderer and therefore produces shapes and an AccessKit tree rather
than pixels. The repository contains no screenshot renderer for this harness. Claiming matching
screenshots from that path would be fabricated evidence; the theme/layout matrix and accessibility
tree are verified directly instead.

## Requirement coverage audit

The nine Editor capability rows now map all 133 requirements to executable evidence or a recorded
M11.e deferral in `tools/roadmap/requirements-coverage.toml`. The map resolves to 71 Rust tests, five
engine tests, 13 proven roadmap criteria, two gates, and 42 explicit deferrals. A deferral names the
missing producer or end-to-end behavior and the test that must close it; it does not upgrade the
capability or turn unfinished work green.

The audit also replaces two claimed surfaces that did not exist. `just run-editor --smoke` now opens
the native desktop shell and closes after three drawn frames, while importer parity is checked by
`tools/editor/import_contract.py` against the importer's own listing instead of a nonexistent CMake
suite. Both checks have recorded behavioral falsifiability proofs.
