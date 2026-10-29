# Add the UI layout editor and a game's menus

## Why

`editor-architecture` requires a UI layout editor among the specialised editors, and `ui-system`
requires that "the Swift DSL, the C++ API, and the visual designer SHALL all produce and consume"
one document. Issue #29 lists "UI layout" as a domain the editor did not have: a `.cyui` document
could be written by hand or by code, and reloaded by a running game when saved, but nothing showed
it while it was edited. `ui-system`'s forcing functions name a settings and input rebinding
interface, and `input-and-actions` requires rebinding with profiles; a Swift game could build
widgets (ABI 1.12) but could not rebind a control, change the display settings or keep a player's
file, so no game's settings screen could be written against the engine.

## What changes

- **The engine's preview service.** `cy::editor::UiLayoutService` answers `ui.catalogue.get` (the
  toolkit's widget kinds and their properties, its style properties, its preview states and binding
  modes) and `ui.preview.render` (a document, the files it reaches, a theme, a size, a scale mode, a
  DPI and one widget's state, drawn by `ui::render::draw_reference`, with every line's laid-out
  rectangle, the diagnostics and the times). The hosted runtime routes `ui.` to it.
- **The editor's UI Layout panel** on the specialised scaffold: a palette of the engine's kinds
  (drag to place), a hierarchy (drag to reparent), the engine-drawn canvas with move, resize and
  anchor handles, an inspector (identity, classes, properties, layout, a binding, inline styles,
  sheets), and a toolbar previewing resolutions and aspect ratios, DPI scales, scale modes, themes
  and states. Thirteen commands (`ui.document.*`, `ui.widget.*`, `ui.theme.save`, `ui.preview`,
  `ui.status`), each an undoable transaction and an MCP tool. A save is the engine writer's text,
  byte for byte, and a running game that loaded the document by path reloads it.
- **ABI 1.15**, thirteen entries: `input_rebind_begin`, `_poll`, `_cancel`, `input_binding`,
  `input_profile_save`, `_load`, `_reset`; `video_get_settings`, `video_set_settings`,
  `video_modes`; `user_data_write`, `user_data_read`; `ui_widget_set_tooltip`. Backends:
  `InputAdapter` (the input server's `RebindOperation`, settled against the bindings in effect, the
  profile as `cyinputprofile 1` text), `VideoSettingsAdapter` and `UserDataDirectory`.
  `UiAdapter::set_document_bindings` hands a module's documents the host's named values. CyberdyneKit
  wraps them (`Menus.swift`).
- **`samples/16-menus`**: a main menu, settings (graphics, audio, controls with rebinding), a pause
  menu and an inventory grid with drag and drop and tooltips, in Swift over documents the editor's
  tools authored, navigable by keyboard, gamepad and pointer. Its host applies every setting to the
  engine and draws the menus on a device at the settings the game applied.
- **Defects fixed on the way**, each with a regression case: a style sheet's selector list (`a, b`)
  dropped all but the first; a document's stale stylesheet survived dropping its `stylesheet` line;
  an unloaded document left its open drop-down list and its dialogs on screen; the editor runtime's
  composite service refused a ninth route; a rebind's slot counted every action before it; a
  shoulder button left focus on a collapsed tab page; a tooltip outlived its hidden widget;
  `ui_set_focus(NULL)` left a widget the toolkit's focus, keeping gameplay suppressed; a rebind's
  conflict check searched other contexts, swapping the interface's own bindings.
- Tests: `integration.editor_backend_ui_layout`, `integration.game_backend_menu`, cases in
  `unit.abi`, `unit.ui`, `unit.ui_toolkit`, `unit.input_actions`, `integration.game_backend_ui`,
  `integration.editor_backend_navigation`, CyberdyneKit's `MenusTests`, the editor's Rust suites
  (the document model, the requests, the commands, two MCP sessions, a panel snapshot), and
  `integration.menus_sample` and `smoke.menus_frames`. The mutations that proved them red are in
  `evidence/`.

## Scope

The canvas previews one widget's state at a time. A theme is edited as text (`ui.theme.save`), not
by a rule editor. The Swift DSL (`WidgetNode`) still makes widgets through the ABI rather than
writing a document. The menus' frame is the pipeline suites' scene, not a level; the GI quality the
settings set reaches the CyberGI system, which that frame does not sample. Fullscreen and v-sync
are recorded by the video adapter; with no window, nothing presents them. The device suites ran on
Vulkan only.
