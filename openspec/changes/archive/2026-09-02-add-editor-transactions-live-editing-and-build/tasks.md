# Tasks: CyberEditor and CyberBuild

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change is archived on that basis. Sections 3 onward are the implementation backlog, sequenced by the
ordering in `design.md`.

## 1. Specification

- [x] 1.1 Record in `design.md`: three worlds rather than two, transactions as semantic operations
      and the five products one operation stream serves, addressing by identity as the payoff of the
      foundations work, interactive and nested transactions, live edit policy plus field
      classification, runtime tweaking versus authoring, one live bridge for three play modes,
      staged and honest hot reload, the plugin boundary being the ABI that already exists, the
      project graph as authority with enforced layering, derivation keys with immutable outputs and
      no undeclared reads, the disposable cache rule, patching already being half-built by the
      content-addressed page formats, the two audit questions, the implementation order, and the
      non-goals
- [x] 1.2 New `editor-documents-and-transactions`: world separation, documents, workspace and view
      state, transactions as the only write path, identity addressing, deltas not snapshots,
      interactive and nested and coalesced transactions, history scope and memory, the journal with
      autosave and recovery, semantic diff and three-way merge, selection and property binding, and
      source control providers
- [x] 1.3 New `live-editing`: the live edit compiler, live edit policy, runtime state preservation,
      asset and shader reload with safe retirement, module reload gated on type ownership, three
      play modes, the live bridge protocol, runtime inspection, tweaking versus authoring, and
      diagnostics
- [x] 1.4 New `project-and-plugins`: the authoritative project graph, modules with public and
      private dependencies, enforced layering, plugins and stable identity, extension points,
      lifecycle phases, the C ABI boundary, type ownership and unload safety, resolution and
      lockfile, trust tiers, and layered typed configuration
- [x] 1.5 New `build-and-packaging`: the build graph, derivation keys, explicit inputs, immutable
      artefacts, deterministic derivation, the derived data cache, the build service, precise
      invalidation, structured diagnostics, distributed execution, packages and install bundles,
      chunk-level patching, staged patch application, manifest integrity, content compatibility,
      downloadable content and mounting, pre-build validation, content audit, and provenance
- [x] 1.6 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 `editor-architecture` — five requirements updated to delegate rather than duplicate: the
      world separation, undo through transactions, plugin extension points with lifecycle owned
      elsewhere, play mode through the live bridge, and build as a client of the build service
- [x] 2.2 `engine-architecture` — the module manifest gains layer, type, and public/private
      dependencies, and layering becomes enforced rather than advisory; registration level and
      architectural layer are distinguished, since they were being conflated
- [x] 2.3 `serialization-and-prefabs` — live prefab update becomes an instance of the general live
      edit contract rather than a parallel mechanism
- [x] 2.4 `asset-import-pipeline` — the cook cache becomes the one derived data cache covering all
      derived data, with the disposability rule and the distinction from the asset registry stated
- [x] 2.5 `core-assets-and-io` — packages gain a manifest with compatibility versions, install
      bundle membership, and content-addressed bulk chunks shared between packages, which is what
      makes chunk-level patching possible
- [x] 2.6 `build-system-and-platforms` — the build service joins the tools, and the editor and
      command line are stated to share one graph and one cache
- [x] 2.7 `thirdparty-dependencies` — the editor model and the build pipeline recorded as
      engine-built, with compilers, codecs, cryptography, and source control integrated beneath;
      and the plugin boundary explicitly reusing the existing C ABI
- [x] 2.8 **Consistency check: no second ABI.** The plugin binary boundary is bound to `native-abi`
      rather than introducing a parallel versioned surface.
- [x] 2.9 **Consistency check: field classification finally has a consumer.** It was introduced with
      the world and authoring work and used only by prefab live update; live editing now derives its
      preservation behaviour from it directly.
- [x] 2.10 `core-type-system`, `core-jobs-and-concurrency`, `core-memory-and-containers`,
      `native-abi` — reviewed; no change needed. Type ownership, cancellation and progress, and
      retirement epochs already provide what these capabilities consume.
- [x] 2.11 **Non-goals recorded**: real-time collaborative editing, and sandboxing of native plugin
      code

## 3. Project and plugins (deferred)

- [ ] 3.1 Project manifest schema, validation, and diagnostics
- [ ] 3.2 Module manifests; public and private dependencies; cycle detection
- [ ] 3.3 Layer declaration and CI enforcement
- [ ] 3.4 Plugin manifests, stable identity, resolution, lockfile
- [ ] 3.5 Extension point registry with independent interface versioning
- [ ] 3.6 Plugin lifecycle phases; failure containment
- [ ] 3.7 Type ownership tracking and unload safety
- [ ] 3.8 Layered typed configuration with layer attribution

## 4. Editor model (deferred)

- [ ] 4.1 Document system; authoring, preview, and runtime world separation
- [ ] 4.2 Transaction manager, typed operations, identity addressing
- [ ] 4.3 Undo and redo with per-document histories and memory budget
- [ ] 4.4 Interactive, nested, and coalesced transactions
- [ ] 4.5 Transaction journal; autosave; crash recovery
- [ ] 4.6 Selection service and property binding; multi-selection editing
- [ ] 4.7 Reflection-driven inspector over the property model
- [ ] 4.8 Workspace, panels, commands, and the command palette
- [ ] 4.9 Domain-specific operation payloads: terrain strokes, foliage paint, graph edits

## 5. Build pipeline (deferred)

- [ ] 5.1 Build graph, derivation keys, immutable artefact identity
- [ ] 5.2 Derived data cache with local, shared, and CI tiers
- [ ] 5.3 Build service with structured protocol, progress, cancellation
- [ ] 5.4 File watching and content-based precise invalidation
- [ ] 5.5 Input declaration verification; undeclared-read detection
- [ ] 5.6 Determinism verification in CI
- [ ] 5.7 Structured diagnostics with navigation
- [ ] 5.8 Target cooking against cook profiles

## 6. Packaging and patching (deferred)

- [ ] 6.1 Package manifests, compatibility versions, install bundles
- [ ] 6.2 Content-addressed chunk storage shared between packages
- [ ] 6.3 Patch manifest generation and chunk-level patching
- [ ] 6.4 Staged, verified, atomic patch application
- [ ] 6.5 Signing and verification; encryption after compression
- [ ] 6.6 Downloadable content mounting with identity resolution and opt-in override
- [ ] 6.7 Pre-build validation suite
- [ ] 6.8 Content audit, size attribution, and cook-time reporting
- [ ] 6.9 Provenance records and symbol archiving

## 7. Live editing (deferred)

- [ ] 7.1 In-editor play mode over the live bridge interface
- [ ] 7.2 Live edit compiler; policy derivation and reporting
- [ ] 7.3 Runtime state preservation from field classification
- [ ] 7.4 Asset, material, and shader reload with handle stability and epoch retirement
- [ ] 7.5 Separate-process play; then remote device play
- [ ] 7.6 Runtime inspection, read-only by default
- [ ] 7.7 Runtime tweak versus authoring edit; the keep-changes flow
- [ ] 7.8 Module hot reload gated on type ownership, for modules that declare it

## 8. Later (deferred)

- [ ] 8.1 Semantic diff and three-way merge
- [ ] 8.2 Source control providers: Git, Perforce, null
- [ ] 8.3 Distributed build execution
- [ ] 8.4 Collaboration — **only after the transaction model has been used in anger**

## 9. Validation (deferred)

- [ ] 9.1 Undo and redo exactness: a document is byte-identical after undoing any operation
- [ ] 9.2 History survives reload, rename, and schema migration
- [ ] 9.3 Out-of-transaction mutation detection catches a deliberately misbehaving tool
- [ ] 9.4 Crash recovery test: kill the editor with unsaved work and recover the journal
- [ ] 9.5 Merge tests: independent changes merge, same-field changes conflict, nothing is discarded
- [ ] 9.6 Live edit tests: authoring change preserves runtime state; policy reported before applying
- [ ] 9.7 Reload safety: a resource replaced during a frame is not freed before its epoch passes
- [ ] 9.8 Broken shader keeps the previous pipeline
- [ ] 9.9 Unload safety: unload with live instances is refused, not permitted
- [ ] 9.10 Layering: an intentional violation fails CI
- [ ] 9.11 Determinism: repeated cooks are byte-identical; a non-deterministic artefact fails CI
- [ ] 9.12 Undeclared input: a node reading a hidden file is detected
- [ ] 9.13 Cache disposability: deleting the cache and rebuilding produces identical artefacts
- [ ] 9.14 Patch tests: a one-texture change produces a small patch; an interrupted patch leaves the
      previous build playable; a corrupt chunk aborts the patch
- [ ] 9.15 Audit: the reference chain for a deliberately included asset is correct

---

**Archived 2026-09-02.** Sections 1 and 2 are complete: `editor-documents-and-transactions`,
`live-editing`, `project-and-plugins` and `build-and-packaging` are in `openspec/specs/`, and seven
existing capabilities were updated — five of them in `editor-architecture`, which now delegates
rather than duplicates. Two consistency findings are recorded there: the plugin binary boundary is
the existing `native-abi` rather than a second ABI, and field classification finally has the general
consumer it was introduced for. The unchecked items from section 3 onward are the implementation
backlog; **collaboration and distributed builds come last deliberately** — both amplify whatever
correctness the transaction and dependency models have, including none.
