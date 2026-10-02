# Tasks

- [x] Extend `Primitive` with the shape material's fields, fold opacity down the tree, skip empty elements, and add `ContentPainter` to `flatten()`.
- [x] Add `cy::ui-text`: `TextPainter` over `TextServer` and the generated built-in font, with its provenance recorded.
- [x] Add `cy::ui-render`: the encoded rows, scissors and draws, the host reference, `ui.slang` with committed SPIR-V and MSL, and `UiRenderer`.
- [x] Add the `UiAndDebug` stage seam (`FrameDescription::ui_stage`, `FrameSinks::ui`).
- [x] Add `cy::ui-console`, the developer console with a virtualised scrollback.
- [x] Pin the frame with no interface to a committed reference drawn before the change.
- [x] Add `unit.ui_text`, `unit.ui_console`, `unit.ui_render` and `render.ui`, extend `unit.ui` and `unit.render_forward`, and prove each case red by a mutation.
- [x] Add the strategy HUD and the console to `samples/13-rts-selection` and publish its images under `docs/design/images/`.
- [x] Update the READMEs, `docs/guides/ui.md`, the requirements map, and validate this change.
