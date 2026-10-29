# Design: the UI layout editor and a game's menus

## The editor never lays out an interface

```
editor (Rust)                                   hosted runtime (C++)
UI Layout panel ─► ui.* commands ─► project text      UiLayoutService
      ▲                 │ save, undo, redo                 │ unload, load (or keep the last good)
      │                 ▼                                  │ apply the state, settle transitions
      │        UiLayoutRequests ── ui.preview.render ────► │ flatten ─► build_draws ─► draw_reference
      └──── picture + node rects + diagnostics ◄────────── ┘
```

The document is the only state the editor owns, and it owns it as TEXT: `ui_document.rs` keeps every
line (widgets, comments, blank lines, directives) and writes exactly what the engine's
`write_document` writes, so a save of an unchanged document is the same bytes, and a diff is the
lines an edit touched. An edit is a command (`ui.widget.add`, `.layout`, ...) that rewrites lines
and saves through `save_graph_source` — the transaction path the other text-backed domains use —
so it undoes, redoes and is an MCP tool by construction.

Everything that depends on how the toolkit lays out, styles and draws comes from the runtime: the
palette is `ui.catalogue.get`'s kinds; the canvas is `ui.preview.render`'s picture; a click selects
by the rectangle the runtime laid the line out at (`NodeRect.line`, set by
`DocumentInstance::widget_at_line`). A preview carries the files the document reaches (`use`d
components, sheets, the theme) read from the project, because the runtime's toolkit must not read
the editor's project directly. The newest preview wins: one request is in flight, and a newer one
replaces any waiting.

The service keeps its own toolkit and font across requests; a request unloads the previous
document and loads the new one, falling back to the last document that parsed so a typo does not
blank the canvas. A previewed state is applied the way a player causes it (the pointer over or
pressed on the widget, focus by navigation, `set_enabled`, `open`), then the toolkit is updated
until its transitions settle, so the picture is the state's end, not its first frame.

## ABI 1.15: what a settings screen needs

Rebinding is the input server's `RebindOperation`, not a key the game reads. `input_rebind_begin`
records the action, scheme, slot and policy for a user; `InputAdapter::observe_pending` offers each
pending device event to it before the server resolves, and `settle` applies the result against the
bindings in effect (`InputUser::binding_control`, `holder_of`), writing overrides into the user's
`BindingProfile`. The conflict search is limited to contexts that bind the rebound action, because
the interface's `UI.*` context binds the same gamepad buttons as any game.

The profile's text form is `cyinputprofile 1` and one line per override:
`<stable id> <keyboard|gamepad> <slot> <path|none> <name>`. Stable ids, not names or indices, so a
renamed action keeps the player's binding and a reordered context does not move it. A line for an
action this build does not declare is skipped.

Display settings belong to the host: `VideoSettingsAdapter` validates a resolution against the
modes it was started with and moves a revision on a change; the renderer (`samples/16-menus`'
`MenusRenderer`) rebuilds its frame when the revision moves, at the new output and the upscaler's
internal extent. Player files are `UserDataDirectory`: one plain name each, written atomically
(`fs::write_atomic`), so a crash mid-write leaves the previous file.

## The sample is authored, not written

`samples/16-menus/project/ui/*` are outputs. `tools/session.py` emits the 253 tool calls that make
them; the MCP test replays the session into a fresh project and compares every file, so the
documents cannot drift from what the tools produce. The game loads them by path, so the scripted
player's `edit` (temporary file, rename) reaches the game exactly as an editor save does.

## Alternatives not taken

- Laying out in the editor with a Rust port of the layout: two layouts disagree eventually, and the
  requirement is that the designer shows what the game shows.
- Rebinding as game code (read the next key, write a binding): it bypasses the input server's
  conflict rules, its per-scheme overrides and the profile, which is what `input-and-actions`
  requires rebinding to use.
