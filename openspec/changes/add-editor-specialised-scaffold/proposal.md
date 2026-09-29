# Proposal: A shared scaffold for the specialised editors

## Why

Issue #29 lists the missing authoring tools: animation, audio, physics, visual scripting, the
sequencer, UI layout, foliage, water, lighting and more. `cy_editor_interface::specialised` already
declares the sixteen domains and the shared surfaces they edit on, but the window has no shared
panel code. The material and VFX graph panels share a canvas only because VFX borrows functions from
`material_graph.rs`. The terrain panel lays out its own header and draws its refusals over the
canvas. Nothing draws a `TimelineSurface`. Without shared groundwork each Wave 1 tool would copy one
of these panels, which is the sixth bespoke editor `editor-architecture` forbids.

## What Changes

- A specialised-editor panel scaffold (`SpecialisedTool` in `cy-editor-shell`): a tool opened by
  `Domain` through `SpecialisedEditors::open`, with a standard header (title, Undo and Redo over the
  document's transaction history), a diagnostics area, and `register_tool`, which refuses a panel
  command that is unregistered, excluded from the MCP projection, or not an undoable mutation.
  `Application::new` runs it for every scaffolded tool. Terrain is ported onto it.
- The node-graph canvas moves out of `material_graph.rs` into a reusable component with a palette
  that hosts any engine-declared vocabulary (`Domain::node_types` or a backend catalogue). The
  material and VFX panels are both ported onto it, and their existing tests are unchanged.
- A shared timeline widget over `TimelineSurface`: ruler and playhead scrub, tracks, keys and clips,
  zoom about the pointer, selection, and Escape to cancel a drag. Each completed gesture is one
  `TimelineEdit` that answers its own inverse. The surface gains `move_key`, `remove_key` and
  `restore_key`, which keep key identity. Only its tests use it for now.
- `editor/README.md` documents how to add a specialised editor.

## Capabilities

### Modified Capabilities

- `editor-architecture`: specialised editors share one panel scaffold, one graph canvas view and one
  timeline view, and every panel command is an undoable MCP tool.

## Impact

Editor-only: `cy-editor-shell` (panels), `cy-editor-interface` (timeline surface), `cy-editor-app`
(registration), and tests in `cy-editor-mcp`. No engine or protocol change. Material and VFX
behaviour is unchanged. Terrain gains the standard header, and its refusal message moves from
paint on the canvas to the diagnostics area.
