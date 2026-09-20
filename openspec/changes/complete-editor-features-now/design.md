# Design

## Context

See `proposal.md` for motivation. The Editor already has a layered Rust workspace, authoritative
services, a command registry, transactions, an optional MCP transport, a project source/build
service, source-control providers, settings, document history, and shared graph/timeline models.
Most missing local features are presentation and orchestration gaps rather than absent domain
models.

Two constraints shape the implementation. First, the window owns `Editor` and may not share mutable
authoritative state with a transport thread. Second, both MCP and SourceKit-LSP use JSON-RPC, while
the current workspace deliberately confines JSON to `cy-editor-mcp`. Adding another wire protocol
requires extracting neutral wire primitives rather than making a source panel depend on MCP.

The parallel Metal workstream owns native viewport transport and renderer integration. This branch
must not edit its files or claim features whose engine-side producer is absent.

## Goals / Non-Goals

**Goals:**

- Turn existing local Editor services into complete command-driven desktop workflows.
- Keep the window responsive and preserve one authoritative `Editor` instance.
- Make human, menu, shortcut, script, and MCP operations use the same commands and transactions.
- Support ordinary Swift packages and SourceKit-LSP without an Editor-specific source format.
- Land work as independently testable vertical slices while maintaining a usable branch.

**Non-Goals:**

- Implementing renderer-produced thumbnails, missing debug render modes, native viewport transport,
  remote-device encoded video, or GPU capture producers.
- Inventing authoring vocabularies or canonical formats owned by unfinished engine subsystems.
- Replacing Xcode, LLDB, or SourceKit; the Editor is a client of those development services.
- Completing the platform build/deployment backend before its M11.d service exists.

## Decisions

### 1. One UI-owned Editor with queued external intents

The desktop process will keep one `Editor` on the interface thread. MCP I/O runs on a background
thread and sends bounded request envelopes to an `AgentConnectionService`. The window drains a
bounded number per frame, invokes the same registry against the same `Editor`, and returns responses
over a per-request channel.

This avoids a mutex around the whole editor, makes human input naturally win by ordering it first in
the frame, and prevents an agent from holding the UI thread. The alternative—running a second
headless `Editor` for MCP—would create two sources of truth and is rejected.

Confirmation becomes an explicit pending request in the service. Reversible commands execute
without prompts; irreversible or external effects remain pending until the window accepts, refuses,
or revokes them. Revocation drops queued work and cancels the active interactive transaction.

### 2. Extract protocol-neutral JSON values and framing

The existing dependency-free JSON value/parser/writer will move from `cy-editor-mcp` into a small
layer-1 `cy-editor-json` crate. MCP keeps all MCP method semantics. A new `cy-editor-sourcekit` crate
owns Language Server Protocol semantics and process management. No JSON type crosses either wire
adapter into services or view models.

Using one audited parser avoids two subtly different JSON implementations. Adding `serde_json` was
considered, but the existing parser already has depth, Unicode, exact integer, and malformed-input
coverage and introduces no new supply-chain dependency.

### 3. Source buffers are presentation state until save

`SourceWorkspaceService` owns file identity, on-disk revision/fingerprint, save conflicts, language
server lifecycle, and build/reload state. `SourceBufferViewModel` owns the in-progress text buffer,
cursor, selection, and diagnostics projection. Editing the buffer does not write the project.

Save invokes the registered source command with an expected disk fingerprint. If the disk changed,
the command returns a structured three-way input (base, buffer, disk) and requires reload, keep, or
merge. This is the same rule for an MCP or external edit; the window never watches itself by special
case.

SourceKit requests are asynchronous and versioned by buffer revision. A late diagnostic or
completion response for an older revision is discarded.

### 4. Document lifecycle belongs to services, not tabs

Tabs render `Workspace::open_documents()` and raise activate/close intents. `DocumentService`
provides a guarded close plan—clean close, save required, or refusal—and `Editor` coordinates the
document and workspace update atomically. The tab never removes a document directly.

Workspace persistence uses a versioned, per-user file under project-local user state. It stores
project-relative document identities and presentation state only. Loading tolerates missing assets
and unknown newer fields; it never writes project content.

### 5. Panels adapt existing services through view models

Settings, source control, and history each receive a view model in `cy-editor-viewmodels` and a thin
shell view. Provider calls and filesystem writes are services/commands, never shell logic.
Source-control refresh runs as an observable operation rather than on every frame. History v1
supports selection plus the existing undo/redo commands; arbitrary time travel is not synthesized.

### 6. Hierarchy operations are commands over stable identities

Selection modifiers update `SelectionService`. Rename and reparent are registered commands that
produce `SetName` and `Reparent` operations. Drag/drop carries node identities, not row indexes, and
the model validates cycles and ordering. Filtering changes presentation only and never changes the
meaning of a range endpoint without naming that it is the visible range.

Unsupported visibility and lock state remain absent until represented by the document and enforced
by manipulation commands. A decorative toggle would lie about persisted behavior.

### 7. Semantic merge extends the operation model before adding UI

Comparison produces identity-keyed changes from document revisions. Three-way merge groups base-to-
local and base-to-incoming changes by semantic target. Independent targets merge automatically;
different results for the same target become typed conflicts. Resolutions produce ordinary
operations committed as one attributed transaction.

The panel only renders `DiffViewModel`/`MergeViewModel`; it does not inspect serialized files or
construct operations itself.

### 8. Vertical slices and ownership boundary

Implementation order is: document lifecycle, operational panels, hierarchy actions, Swift
Workspace, concurrent MCP, semantic merge, then asset-browser completion. Each slice includes its
commands, headless tests, shell regression tests, and documentation before the next begins.

This sequence delivers usable features early and avoids requiring the most cross-cutting thread and
wire changes before the document workflows they expose exist.

## Risks / Trade-offs

- **MCP request waits for a UI frame** → Use bounded channels, explicit deadlines, and progress
  responses; never wait while holding Editor state.
- **A language server emits stale results** → Tag every request and publication with document
  version and discard obsolete responses.
- **SourceKit is absent or incompatible** → Keep plain editing/build/save available and publish a
  structured availability reason and remedy.
- **Workspace restoration opens removed assets** → Skip missing documents, retain the rest, and
  post one actionable notification.
- **Provider operations block** → Execute refresh/history/provider commands as observable
  background operations and apply results by revision.
- **Broad scope creates long-lived integration risk** → Keep slices independently testable and
  avoid editing Metal/viewport-transport ownership areas.
- **Semantic merge exposes unsupported operation kinds** → Refuse those conflicts explicitly;
  never fall back to line merge or silently choose a side.

## Migration Plan

1. Add new services and view models without changing the default layout or persisted formats.
2. Introduce versioned workspace/source state readers that accept absence as the old state.
3. Add panels and tabs behind registered View commands, then include stable panels in the default
   workspace after interaction tests pass.
4. Refactor JSON into the neutral crate with byte-for-byte MCP regression coverage before adding the
   SourceKit client.
5. Enable desktop MCP after headless MCP and desktop suites both pass through the shared executor.

Rollback removes newly registered panels and restores the previous MCP executable path; project
assets remain compatible because only per-user state gains new versioned records.
