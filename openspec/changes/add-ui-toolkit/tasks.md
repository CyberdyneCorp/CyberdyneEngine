# Tasks

- [x] Add `cy::ui-toolkit`: the widgets as elements with part records, their states, events, focus (directional, tab order, ring), the built-in theme, transitions, animations, bindings, the text field's editor, the virtualised list, overlays (drop-down list, tooltip, dialog backdrop); `unit.ui_toolkit`.
- [x] Add `.cyui` documents: parser and writer, `use` with parameters, `DocumentInstance` with diffing reload, `DocumentHost` over the VFS watcher, the cooked bundle and `BundleResolver`; cases in `unit.ui_toolkit` and `integration.ui_toolkit`.
- [x] Add `cy::ui-input`: the `UI.*` actions, default bindings, `UiInputBridge` and the focus layer; `unit.ui_input`. Fix the input server's shadow chain within one context, with a regression case in `unit.input_actions`.
- [x] Extend `cy::ui`: opacity groups, the render transform and intensity in `flatten()`, focus next and previous, navigation past collapsed ancestors, premultiplied `.cyss` colours, absolute margins; cases in `unit.ui`.
- [x] Draw opacity groups offscreen and add the HDR stage (`FramePassKind::UiHdr`, `FrameSinks::ui_hdr`, `scene_scale`); regenerate the committed SPIR-V and MSL; `render.ui_toolkit` with the widget gallery against golden images.
- [x] Add the build graph's `ui` producer and its cases in `integration.build_content`.
- [x] Append ABI 1.12 (`ui_*` toolkit entries, `UiBackend` virtuals, thunks, `unit.abi`), implement them in `UiAdapter` with `integration.game_backend_ui` cases, regenerate the Swift and Rust overlays and the baseline.
- [x] Add CyberdyneKit's `Widget`, documents, `UIAnimation`, `WidgetNode`/`mountWidgets` and `UIWidgetBinding`, and `UIToolkitTests`.
- [x] Prove the new cases red by mutation and record them in `evidence/`.
- [x] Update the READMEs, `docs/guides/ui.md` and `docs/guides/swift.md`, the images, the requirements map, and validate this change.
