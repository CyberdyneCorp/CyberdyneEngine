# Design: specialised-editor scaffold (#29 Wave 0)

## Context

The editor is a client of the engine. Authoring goes through registered commands, each running
inside a document transaction, and the MCP tool list is a projection of the registry. The
specialised-editor model (`cy_editor_interface::specialised`) owns the domains and the shared
surfaces, and `SpecialisedEditors::open` is the only way to borrow one.

## Decisions

### A tool is a trait implementation, not a panel function

`SpecialisedTool` has associated constants (`DOMAIN`, `TITLE`, `COMMANDS`), a `Target` type, and
three functions: `target` (resolve what is edited or draw the empty state), `diagnostics` (held
refusals) and `body` (draw on the open `Session`). `specialised::show::<T>` does the rest in a fixed
order: header, target, diagnostics, open, body. `target` runs before `open`, so an empty state
leaves the reserved region as it was, which is the terrain panel's existing behaviour. Associated
constants make `COMMANDS` checkable without drawing a frame.

### Parity is checked where the registry is built

`register_tool` projects each of `COMMANDS` through `cy_editor_agent::tool::project_one`, the same
projection the MCP server lists. It refuses a missing command, an agent exclusion, or an effect
class other than `Read` or `ReversibleMutation`. `Application::new` calls
`register_specialised_tools` after every other registration, so a tool that invokes a command an
agent cannot reach, or one that bypasses undo, does not start. MCP parity therefore follows from
the registry, and a test for each tool drives its commands over the wire.

### The canvas is moved, not rewritten

The canvas code is moved from `material_graph.rs` unchanged, apart from visibility, the egui id
prefixes (`graph-*` instead of `material-*`, which nothing observes), and two new palette helpers.
The material panel keeps its own labels and stage filter, and the VFX panel keeps its `vfx.` label
stripping. Both now draw through `node_palette` and place nodes with `palette_slot`, which is the
grid both already used (224-point columns, 150-point rows). The canvas internals the material tests
call are imported under `#[cfg(test)]`, and `material_graph` re-exports the two gesture types the
VFX tests name, so no existing test changes.

### The timeline reads the surface; the host writes it

`timeline::show` takes `&TimelineSurface`, so it cannot mutate one. It answers a scrub position
(preview state, not a transaction) and a list of `TimelineEdit`s. A key drag or clip-edge drag is
held in `TimelineView` and produces one edit on release, and Escape drops it. `TimelineEdit::apply`
returns the inverse edit, so a host can record it as one transaction and an MCP command can replay
it. `restore_key` puts back a removed key with its identity. Identities are never reused, so the
surface's ordinal counter is the one field an undo does not restore.

### Terrain's refusal moves to the diagnostics area

The terrain panel drew `terrain_problem` as painter text on the canvas, which is invisible to
accessibility and a second convention for refusals. It now shows as a glyph-and-word status row in
the scaffold's diagnostics area. The standard header adds the "Terrain" title above the brush
controls. Every command and gesture is unchanged.

### A layout bug found while porting Terrain

The terrain body put its paint field inside `ui.allocate_ui` within the row that holds the controls.
`allocate_ui` inherits the parent's horizontal layout, so the heading, the hint and the field sat
side by side, and the field got the strip left over at the right edge: 98 points wide in a
900-point panel. This is on `main` too (`docs/design/images/editor-terrain-before.png`). The field
is now laid out top-down. It carries an accessible label, "Terrain brush field", and
`the_terrain_brush_field_fills_the_space_beside_the_controls` checks its width.

### Panel snapshots without a window

`editor:window?panel=<kind>` captures a panel from the live window, but only the front tab, and no
command selects a tab. `cy-editor-shell/tests/panel_snapshots.rs` (ignored by default) renders one
panel through `Panels::ui` and the egui-wgpu renderer into an offscreen texture on any wgpu adapter.
The material and VFX panels render pixel-identical before and after the canvas moved.

## Found, not fixed here

The status glyphs in `cy_editor_visual::colour::Semantic::glyph` (for example `✕` for Error) and the
palette's `＋` are not in egui's default fonts, so they render as a missing-glyph box in the window
and in the snapshots. The glyph table is part of the visual language and is shared by every panel,
so it belongs in its own change.

## Not in this change

Wave 1 tools, and any engine or bridge change. The timeline has no curve-value editing and no
snapping, and it does not show the surface's loop range. The sequencer and animation editors own
those surfaces and add them when they need them.
