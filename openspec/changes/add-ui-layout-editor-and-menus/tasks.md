# Tasks

- [x] Add `cy::editor::UiLayoutService` (`ui.catalogue.get`, `ui.preview.render`), the wire fixtures, `integration.editor_backend_ui_layout`, and route `ui.` in the editor runtime.
- [x] Fix the defects the preview found: selector lists in style sheets, a stale sheet on reload, a dropped document's drop-down list and dialogs, the composite service's route limit; each with a regression case.
- [x] Add the editor's UI document model (`ui_document.rs`), the requests (`ui_layout_requests.rs`), the thirteen `ui.*` commands, their undo, their MCP tools, and the UI Layout panel with its palette, hierarchy, canvas handles, inspector and toolbar.
- [x] Add the MCP sessions: a document laid out, previewed and undone; the menus sample authored by the tools and compared with the committed files.
- [x] Append ABI 1.17 (rebinding, profiles, video settings, player files, tooltips), the thunks and `unit.abi`'s cases; regenerate the Swift and Rust overlays and the baseline.
- [x] Add the backends: rebinding in `InputAdapter` (with the per-action slot fix in the input user), `VideoSettingsAdapter`, `UserDataDirectory`, `UiAdapter::set_document_bindings`, and `integration.game_backend_menu`.
- [x] Add CyberdyneKit's `Menus.swift` and `MenusTests`.
- [x] Add `samples/16-menus`: the session, the documents and themes, the Swift game, the host, the renderer, the scripted player, `integration.menus_sample` and `smoke.menus_frames` with goldens.
- [x] Fix what the sample found: focus on a collapsed tab page, a tooltip outliving its widget, a null focus leaving a widget focused, a rebind conflict across contexts, documents outside `game/` unlisted, the panel's column layout; each with a regression case where it is testable.
- [x] Prove the new cases red by mutation and record it in `evidence/`.
- [x] Update the READMEs, `docs/guides/ui.md` and `swift.md`, the requirements map, the matrix and the living specifications; validate this change.
