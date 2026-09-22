# Design

## Context

The importer already emits stable sub-asset identities, the editor already owns a command registry
and observable operation model, and the material vertical slice already establishes asynchronous
backend-service envelopes. The missing work is primarily composition: preserving stable identities
from import through document references, deriving controls from backend schemas, and driving runtime
preview state without introducing private editor shortcuts.

## Goals / Non-Goals

**Goals:**

- Complete each workflow from visible editor gesture to runtime-visible result.
- Keep commands as the single mutation surface used by UI and agents.
- Keep long operations off the interface thread and cancellable at honest step boundaries.
- Preserve stable asset, sub-asset, node, pin, request, preview, and artefact identities.
- Reuse shared graph, painting, diagnostics, progress, transaction, and backend-service facilities.

**Non-Goals:**

- A general DCC replacement, arbitrary terrain volumetrics, or CPU-authored particle runtime.
- Importing files outside the project without first copying them into a project-owned source path.
- Linking third-party importers, renderer backends, shader compilers, Metal, or VFX lowering into the
  Rust editor.

## Decisions

### 1. File selection and file drop normalize into one import queue

The shell copies selected external files and their declared companions into a project-owned import
folder, then submits the existing `asset.import` command through a background operation. File-dialog,
drop, command, and agent paths therefore converge before cooking. Each queued item has a request ID,
progress row and cancellation token; cancellation is cooperative and leaves completed sidecars and
cache entries valid.

### 2. Imported sub-assets are first-class catalogue entries

The importer result carries every sub-asset's stable ID, kind, display name and source. The editor
catalogue indexes those logical children separately from the source file. References serialize the
stable sub-asset identity plus readable source/name migration metadata. A model source whose primary
asset is a prefab is instantiated recursively; mesh nodes retain their extracted material bindings.

The importer/editor JSON contract is schema version 3. Each logical asset carries `id`, `kind`,
`name`, `source` and a deterministic dependency list. Each prefab mesh node carries a `materials`
array indexed by the mesh section's stable material-slot number. Older version 1 and 2 results
remain readable. The editor persists all imported slots in the authoring-only
`ImportedMaterialSlots` component and mirrors slot zero onto `MeshRenderer.material` for ordinary
runtime compatibility.

### 3. Material controls are entirely catalogue-driven

Property descriptors carry stable identity, value kind, default, optional numeric bounds/step,
semantic role, asset-kind constraint, enum choices and tooltip. The editor maps descriptor kinds to
shared controls. A texture picker queries the project asset catalogue by the descriptor's required
asset kind. No node-name switch is permitted in the editor.

Catalogue payload schema 2/catalogue version 3 carries those fields separately, plus the consuming
stage, graph domain, vector lane count and required target-capability bits. Schema 1 remains readable.
Authored values are retained by stable node and property identity; readable property names remain
migration metadata and are resolved from the current catalogue when compiling.

### 4. Compilation publication drives preview reload

A successful compile result is not considered visible merely because it returned an artefact ID.
The editor creates or reuses a session-scoped preview world, requests reload with the compiled
identity, waits for an acknowledgement containing requested and applied identities, and only then
marks the viewport current. Stale acknowledgements are ignored by request and preview generation.

Reload requests also carry the selected scene entity identity and material-slot index for every
target renderer. Acknowledgements echo the exact bindings; the editor does not report the viewport
current when an acknowledgement omits or changes a target. Runtime parameter messages address the
preview generation, applied artefact generation and stable parameter identity, followed by an
explicit bool, integer, float, vector or texture-asset value kind.

The editor backend delegates artefact publication, preview creation, reload, typed parameter
updates and destruction through an engine-owned `MaterialPreviewRuntime` interface implemented by
the viewport host. Session state is committed and an acknowledgement is emitted only after that
runtime accepts the operation. This prevents a protocol-level echo from being mistaken for a GPU
program installation and keeps renderer/Metal types out of the ABI and Rust editor.

### 5. Terrain edits are modifiers, not destructive heightmap writes

Heightmaps import into tiled terrain source data. Sculpt and paint gestures append or edit stable
modifier/layer records in one transaction per gesture. Evaluation and GPU upload occur asynchronously;
cancellation may discard unpublished evaluation but never authored modifiers.

### 6. VFX reuses the service and graph contracts

VFX adds schemas and operations to the existing generic service dispatcher. Its catalogue uses the
same stable graph identities and typed properties as materials, while lowering remains VFX-owned and
returns attribute layout, kernels, generated source, costs, dependencies and target capabilities.
The editor consumes those results without linking the lowering implementation.

## Sequencing

Implement in the user-visible dependency order: import UI/queue; sub-assets/prefabs; material
properties; material preview/reload; terrain; VFX. A later slice may start only after the preceding
slice has an end-to-end test and its visible failure/cancellation path is covered.

## Risks / Trade-offs

- Companion-file copying can be ambiguous; importer-declared dependencies are copied only when they
  resolve beside the selected source, and unresolved dependencies remain structured diagnostics.
- Large prefab instantiation can create large transactions; one transaction preserves atomic undo,
  while the operation model keeps construction off the interface thread until commit.
- Terrain evaluation and VFX compilation may have non-cancellable GPU/compiler regions; progress
  reports the boundary and publication remains the commit point.

## Migration Plan

Existing source-path mesh references and name-only material properties remain readable. On save they
gain stable sub-asset/property identities when resolution is unambiguous. Existing command and live
message tags remain unchanged; new service operations and schema versions are additive.
