# Tasks: Editor application, presentation architecture, and developer workflow

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change was archived on that basis.

Sections 3 to 10 record the implementation the decision implies and are **deliberately deferred to
implementation changes**. They are listed so the scope is not lost.

The risk concentrates in section 4. Until the editor talks to a hosted runtime over the SDK and the
protocol, every feature built above is built against the wrong shape; see `design.md` §9, which
states plainly that phase 2 is the milestone that matters.

## 1. Specification

- [x] 1.1 Record the reversal of "the editor is an engine application built on CyberUI", its
      rationale, and the cost accepted, in `design.md` §1
- [x] 1.2 Add `editor-rust-application`: the Rust client boundary, three hosting modes with hosted
      as the production default, workspace structure and dependency direction, safety and
      interoperation rules, MVVM with services and commands, the two hard rules (a view model is
      never a second source of truth; no view model depends on another panel's view model), the
      command registry as the single action surface, asynchronous operation, change propagation,
      the editor state model, the editor SDK, the plugin surface, toolkit independence,
      testability, and forbidden patterns
- [x] 1.3 Add `editor-ui-ux`: familiarity as a feature with a Unity keymap, docking and
      workspaces, density over decoration, command palette and search, keyboard-first operation,
      the reflection-generated inspector with multi-selection, every edit as a transaction, problem
      surfacing, long operations, notifications, theming and accessibility, declared interaction
      performance targets, protection against losing work, discoverability, editor self-profiling,
      and forbidden patterns
- [x] 1.4 Add `editor-viewport-and-gizmos`: the rendering responsibility split ("the editor decides
      what should be shown, the renderer decides how it is drawn"), viewport transport across
      local surface, shared texture and encoded stream, multiple viewports, navigation, engine-side
      picking with stable identity, gizmos and numerically stable manipulation, snapping and
      precision, view modes and debug visualisation, overlays, editing while playing, degradation,
      view state capture, and forbidden patterns
- [x] 1.5 Add `developer-workflow-and-just`: one entry point, `just` orchestrates and does not
      build, the recipe surface, profiles consistent across four toolchains, `just doctor`,
      reproducible environments, continuous integration invoking the same recipes,
      cross-compilation and device deployment, reproducible code generation, graduated test
      recipes, one-command diagnostics bundles, honesty about destructive effects, local overrides,
      self-documenting recipes, and forbidden patterns
- [x] 1.6 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 `ui-system` — "Engine-owned UI system" rewritten (in-game tooling replaces the editor as
      the third consumer); **removed** "Editor UI shares the runtime UI" with a recorded supersession;
      added "Forcing functions for the UI system" (shipped in-game tooling, a conformance suite,
      and a real sample interface) which states in the specification itself that this is weaker
      than dogfooding an editor; added "The runtime UI system is not the editor's toolkit". Purpose
      text corrected.
- [x] 2.2 `editor-architecture` — "Editor is an engine application" rewritten: the editor is a
      separate Rust binary, and the four-world separation is now split across processes;
      "Plugin architecture" rewritten for editor SDK abstractions, the three plugin forms, and the
      prohibition on Rust's native binary interface as a contract
- [x] 2.3 `live-editing` — "Play modes" rewritten: no play mode runs in the editor process,
      `InEditor` now denotes iteration speed rather than co-location, and a runtime crash leaves
      the editor intact
- [x] 2.4 `native-abi` — added "Rust SDK overlay", generated from the same ABI description as the
      Swift overlay, with `unsafe` confined to the overlay crate
- [x] 2.5 `project-and-plugins` — "The plugin binary boundary is the engine's C ABI" extended to
      the editor: source-distributed Rust plugins may use the SDK crates; binary ones cross the C
      ABI or a protocol
- [x] 2.6 `build-system-and-platforms` — added "Rust toolchain integration": pinned version,
      Cargo owns Rust compilation, profile mapping across toolchains, editor-only builds against a
      prebuilt engine library
- [x] 2.7 `thirdparty-dependencies` — added "Rust editor dependencies": same policy as C++
      dependencies, transitive cost counted, the interface toolkit an integration rather than an
      architecture
- [x] 2.8 `diagnostics-profiling-and-crash` — reviewed; no requirement change needed. Editor
      self-profiling and the reproduction bundle use the existing trace schema and artefact
      format; `editor-ui-ux` and `developer-workflow-and-just` reference them rather than
      duplicating them.
- [x] 2.9 `editor-documents-and-transactions` — reviewed; no requirement change needed. The
      transaction model is unchanged by the editor's language and process; the new capabilities
      route every edit through it.
- [x] 2.10 `reflection-system` — reviewed; no requirement change needed. The inspector consumes
      existing reflection metadata through the Rust SDK.
- [x] 2.11 `world-partition-and-streaming` — reviewed; no requirement change needed. Viewport
      selection uses the existing persistent entity identity.

## 3. Editor SDK and protocol (deferred to implementation)

- [ ] 3.1 ABI description as the single source for the Swift and Rust overlays
- [ ] 3.2 Rust SDK crate: generation-checked handles, typed errors, string and buffer contracts
- [ ] 3.3 Registered callback mechanism with re-entrancy rules
- [ ] 3.4 Regeneration determinism check wired into continuous integration
- [ ] 3.5 Protocol client: connection, versioning, reconnection, backpressure

## 4. Runtime host and hosting modes (deferred to implementation)

- [ ] 4.1 Runtime host process: lifecycle, supervision, restart, crash artefact handover
- [ ] 4.2 Hosted mode as the default path, with `NoRuntime` and `Embedded` behind the same services
- [ ] 4.3 Remote host over the live bridge, including console deployment
- [ ] 4.4 Editor survival and recovery on runtime failure

## 5. Editor application skeleton (deferred to implementation)

- [ ] 5.1 Cargo workspace and crate graph with enforced dependency direction
- [ ] 5.2 Service layer: documents, project, workspace, selection, transactions, assets, runtime
      session, notifications
- [ ] 5.3 Command registry with availability predicates and binding resolution
- [ ] 5.4 View model layer and change propagation, with headless tests from the start
- [ ] 5.5 Toolkit abstraction layer and the first toolkit implementation behind it

## 6. Core panels (deferred to implementation)

- [ ] 6.1 Hierarchy, inspector, project browser, console, problems view
- [ ] 6.2 Reflection-generated inspector with custom editors and multi-selection
- [ ] 6.3 Docking, workspaces, and session restore
- [ ] 6.4 Command palette and unified search

## 7. Viewport (deferred to implementation)

- [ ] 7.1 Transport abstraction with the local-surface implementation
- [ ] 7.2 Shared-texture transport for the hosted local case
- [ ] 7.3 Encoded-stream transport with input forwarding for remote and console
- [ ] 7.4 Frame view state carried with the image; picking resolved against the presented frame
- [ ] 7.5 Navigation, engine-side picking, selection service integration
- [ ] 7.6 Gizmos: engine-side geometry and drawing, editor-side intent and manipulation state
- [ ] 7.7 Snapping, numeric entry, precision at large coordinates
- [ ] 7.8 View modes and debug visualisation routed to engine debug views
- [ ] 7.9 Degradation policy and staleness indication

## 8. Developer workflow (deferred to implementation)

- [ ] 8.1 `justfile` with the recipe surface and consistent naming
- [ ] 8.2 `just doctor` covering every required toolchain with corrections
- [ ] 8.3 Profile mapping across CMake, Cargo, the shader compiler, and the content pipeline
- [ ] 8.4 Graduated test recipes with stated durations
- [ ] 8.5 Continuous integration invoking the recipes rather than duplicating them
- [ ] 8.6 Diagnostics bundle recipe
- [ ] 8.7 Local override file and reporting of active overrides

## 9. CyberUI forcing functions (deferred to implementation)

- [ ] 9.1 Developer console as a shipping feature
- [ ] 9.2 Gameplay and network debuggers with virtualised tables
- [ ] 9.3 Profiler and statistics overlays
- [ ] 9.4 Settings and input rebinding interface
- [ ] 9.5 CyberUI conformance suite with performance assertions in continuous integration
- [ ] 9.6 Sample project with a complete, real interface

## 10. Enforcement (deferred to implementation)

- [ ] 10.1 Check for C++ types, raw-pointer identity, and stray `unsafe` in editor crates
- [ ] 10.2 Check for peer dependencies between panel crates
- [ ] 10.3 Check that toolkit types do not appear in plugin-facing or protocol interfaces
- [ ] 10.4 Interaction performance regression tests against the declared targets
- [ ] 10.5 Check that every documented developer procedure has a recipe
