# Editor transactions, live editing, and the build pipeline

## Why

The engine has 52 capabilities describing what it renders, simulates, streams, and ships. What it
does not yet describe is how a team **works on it every day**: how an edit is recorded, how a
change reaches a running game, how a project declares what it is made of, and how source becomes a
shippable, patchable product.

Four gaps, each of which becomes structural if left:

**1. Editor mutation has no model.** `editor-architecture` requires that all editor mutations go
through undo/redo, which is right and stops short of the part that matters: *what an operation is*.
Without a document model, stable object addressing, and delta-based operations, undo becomes a
collection of per-tool snapshot hacks, and everything built on it — diff, merge, autosave, crash
recovery, collaboration — has to be invented separately later.

**2. Live editing is asserted but not specified.** Play mode exists, prefab live update exists, and
hot reload is mentioned in several capabilities. Nothing says what may safely be applied to a
running world, what must restart, or what happens to a character's current health when a designer
changes its prefab's maximum. The field classification introduced with the foundations work exists
precisely to answer that and has no consumer.

**3. The project is inferred from folders.** Modules are described as directories with manifests,
and architectural layering — that a runtime module may not depend on an editor module — is a
convention nothing enforces. Dependency direction is the property most expensive to recover once
lost.

**4. Building is a set of steps rather than a graph.** Cook profiles, a cook cache, code generation,
and distribution artefacts are each specified; the dependency graph that connects them is not. So
incremental correctness rests on timestamps and habit, and the patching story — which the
content-addressed page formats have already half-built — has nothing to complete it.

The four contracts this change locks in:

> **All persistent editor mutation is expressed as schema-aware transactions against document
> models.** Widgets and tools never own project state.

> **Live editing propagates validated semantic deltas from authoring models into runtime worlds.**
> It never assumes authoring state and simulation state are the same, and preserves runtime state
> unless a policy says otherwise.

> **Every derived build artefact has explicit inputs, a deterministic derivation key, and immutable
> output identity.** Incremental builds are dependency-driven, not timestamp-driven.

> **Engine extension occurs only through versioned module and plugin contracts.** First-party
> modules use native C++; externally distributed binary plugins cross the existing stable C ABI.

## What changes

**`editor-documents-and-transactions`** — authoring, preview, and runtime worlds kept distinct;
documents as the unit of editing, decoupled from files; workspace and view state separated from
asset content; transactions as the only path for persistent mutation, addressing objects by the
stable identities the foundations work established; delta operations rather than snapshots;
interactive, nested, and coalesced transactions; an undo memory budget with domain-specific payloads
so a terrain stroke does not copy a landscape; the transaction journal that serves autosave, crash
recovery, and review at once; semantic diff and three-way merge; and a source control provider
interface.

**`live-editing`** — the live edit compiler translating authoring deltas into runtime deltas; a
declared **live edit policy** per field and component; runtime state preservation driven by the
existing field classification; asset, shader, and material hot reload with handle stability and
GPU-safe retirement; module hot reload as a controlled capability gated on type ownership rather
than assumed; three play modes including remote devices; the live bridge protocol and remote runtime
inspection; and the distinction between tweaking a running game and editing a project, with an
explicit path from one to the other.

**`project-and-plugins`** — the project graph as the authority rather than folder convention; module
manifests, types, and public versus private dependencies; **enforced architectural layering**;
plugin packages, stable plugin identity, extension points, lifecycle phases, and dependency
resolution with a lockfile; the plugin ABI boundary bound to the **existing `native-abi`** rather
than a second one; type ownership and unload safety; and layered typed configuration.

**`build-and-packaging`** — the build graph as a DAG of nodes with deterministic derivation keys and
immutable outputs; explicit inputs with undeclared reads treated as defects; the derived data cache
generalised across imports, shaders, and build outputs, and declared disposable; the build daemon
with a structured protocol and diagnostics; precise invalidation from file watching; distributed
execution; deterministic cooking; package chunking and install bundles; **chunk-level patching**,
which the content-addressed geometry and texture page formats already make granular; patch staging
and atomic switch; signing; DLC and mod trust tiers; pre-build validation and the asset audit that
answers *why is this in my build*; and build provenance with symbol management.

## Impact

- **New**: `editor-documents-and-transactions`, `live-editing`, `project-and-plugins`,
  `build-and-packaging`
- **Modified**: `editor-architecture` (undo, plugins, play mode, and the world separation delegate
  to the new capabilities), `engine-architecture` (module system under the project graph),
  `serialization-and-prefabs` (live prefab update as an instance of the general contract),
  `asset-import-pipeline` (the cook cache becomes the derived data cache),
  `core-assets-and-io` (package manifests, chunk sharing, and compatibility),
  `build-system-and-platforms` (the daemon among the tools), `thirdparty-dependencies`
- **Non-goals, recorded**: real-time collaborative editing (the transaction log makes it possible;
  it is not specified), and sandboxing of native plugin code, which cannot be honestly promised
  in-process
