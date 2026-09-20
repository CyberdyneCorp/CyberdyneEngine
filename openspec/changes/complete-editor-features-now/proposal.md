# Proposal

## Why

CyberEditor already has strong document, command, transaction, MCP, settings, source-control, and
Swift-build foundations, but several of those capabilities are reachable only from tests or
headless commands. The current project state can support a complete local authoring loop without
waiting for the renderer, remote-device transport, or future engine vocabularies, so those usable
Editor features should now be finished as one coherent workstream.

## What Changes

- Run the existing MCP agent interface alongside the desktop window, with visible connection,
  activity, scope, budget, confirmation, pause, and revocation controls.
- Add a Swift Workspace for project source navigation, tabbed editing, SourceKit-LSP diagnostics,
  source navigation, build, and development-module reload.
- Expose open documents as tabs with activation, dirty state, guarded close, and restored per-user
  workspace state.
- Complete currently model-supported hierarchy authoring: modifier/range multi-selection, inline
  rename, transactional drag-and-drop reparenting, and explicit refusal for unsupported lock or
  visibility state.
- Add usable Settings, Source Control, and Undo History panels over the services and view models
  that already exist, keeping provider-specific and authoritative state out of views.
- Complete asset-browser operations that do not require rendered previews: folders and filters,
  rename/move with metadata preservation, drag/drop intents, and import-settings access.
- Add semantic document comparison and three-way merge presentation over the existing operation and
  document model, with explicit conflict resolution and no silent loss.
- Add regression, interaction-target, persistence, accessibility, and command/MCP parity coverage,
  and update Editor documentation as behavior lands.
- Explicitly defer renderer-produced thumbnails and previews, remote-device encoded streaming,
  renderer/debugger views whose producers do not exist, build/deployment service integration,
  domain editors lacking engine authoring vocabularies, and platform work owned by the active Metal
  workstream. These are dependencies, not placeholder controls.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `editor-agent-interface`: require the shipped desktop window and MCP transport to operate
  concurrently with visible, revocable human control.
- `editor-architecture`: add the Swift Workspace and make the already-required project, hierarchy,
  source-control, history, settings, and asset operations usable desktop surfaces.
- `editor-documents-and-transactions`: make open-document state, guarded close, restoration,
  attributed history, semantic comparison, and merge directly operable in the workspace.
- `swift-scripting`: define the embedded SourceKit-LSP editing and edit-build-reload experience
  while retaining ordinary Swift package compatibility with external tools.

## Impact

The primary changes are in `editor/crates/cy-editor-app`, `cy-editor-shell`,
`cy-editor-interface`, `cy-editor-viewmodels`, `cy-editor-services`, `cy-editor-documents`,
`cy-editor-agent`, and `cy-editor-mcp`. SourceKit-LSP remains an external development tool behind an
Editor-owned client boundary; no Swift or MCP type enters document, command, or view-model layers.
Project content formats and the engine ABI remain compatible. Work avoids the viewport transport,
RHI, native Metal, renderer, and backend files owned by the parallel workstream.
