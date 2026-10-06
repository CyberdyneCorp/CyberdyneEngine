# Tasks

## 1. ABI 1.6

- [ ] 1.1 Append the entries, structs, enums and the `ui_event` vtable member to `cy_abi.h`; `CY_ABI_MINOR` 6; layout asserts; `just quality-abi` classifies it as appends; baseline, Swift overlay and Rust SDK regenerated.
- [ ] 1.2 `UiBackend`, the thunks, `GameServices::ui`; `unit.abi` cases for the phase rule, the unbound backend, each entry's argument checks, the table shape and the 1.6 layouts (`test_game_ui.cpp`, `test_layout.cpp`).
- [ ] 1.3 `BehaviourRuntime::ui_event`, with the pre-1.6 vtable prefix rule.

## 2. Adapter

- [ ] 2.1 `cy::game-backend-ui`: `UiAdapter` over the store, the text painter and an owned `Interaction`; ownership, kinds, progress fill, visibility, clicks, hits and focus (`integration.game_backend_ui`).

## 3. CyberdyneKit

- [ ] 3.1 `UI.swift`: elements, layout and style with the zero-means-default conversion, events, hit test and focus.
- [ ] 3.2 `UIBuilder.swift`: nodes, modifiers, the result builder, `mountUI`/`unmountUI`, `UITree`.
- [ ] 3.3 `ui_event` thunk, `onUIEvent`, `CallbackSet.uiEvent` and the macro's mapping (`UITests`).

## 4. Sample, docs, proofs

- [ ] 4.1 `samples/13-rts-api`: `Hud.swift`, the commander's HUD and Build button, the hit-test guard, `HudShowcase`; the host's interface and `--no-ui`; the integration test and its control.
- [ ] 4.2 `render.rts_api_hud`: the stream and the device frame against the C++ HUD, and the golden with the button.
- [ ] 4.3 `src/abi/README.md`, `bindings/swift/README.md`, `docs/guides/swift.md`, `docs/guides/ui.md`, `src/ui/README.md`, the sample's README.
- [ ] 4.4 Build, run the suites, and record mutation proofs in `evidence/falsification.md`.
