# Let a Swift game build and drive a CyberUI interface (ABI 1.6)

## Why

#102 put CyberUI on screen — the element store, layout, text in a built-in font, the interface pass
and a developer console — and its own proposal records what it left out: "Swift access to the
interface". A Swift game could reach input, cameras, physics, navigation, audio, spawning and the
scheduler through the ABI, and not one element: the strategy HUD in `samples/13-rts-selection` is
C++ game code because nothing else could build it. Issue #91 asks for the follow-up: append the
entries, wrap them in CyberdyneKit, and rewrite that HUD in Swift.

## What changes

- **ABI 1.6** (`native-abi`): fourteen appended entries — `ui_root`, `ui_create`, `ui_destroy`,
  `ui_set_layout`, `ui_set_style`, `ui_set_text`, `ui_set_image`, `ui_set_progress`,
  `ui_set_visibility`, `ui_set_opacity`, `ui_element_rect`, `ui_hit_test`, `ui_focus`,
  `ui_set_focus` — the structs `CyUiElementDesc`, `CyUiLayout`, `CyUiStyle` and `CyUiEvent`, the enums
  for kinds, layout models, directions, justification, alignment, visibility and event kinds, and
  one vtable member, `CyBehaviourVTable.ui_event`, through which a click on a button reaches the
  behaviours of the entity that owns it. `CY_ABI_MINOR` 6. Every entry is `[N U]`: the interface is
  presentation.
- **The seam**: `cy::abi::game::UiBackend` (`include/cy/abi/game/ui.h`), thunks in
  `src/abi/src/game/ui_thunks.cpp`, `BehaviourRuntime::ui_event`. `cy_abi` names no part of CyberUI.
- **The adapter**: `cy::game-backend-ui` (`src/game_backend/ui/`), behind `CY_UI`: `UiAdapter`
  implements the backend over an `ElementStore`, a `TextPainter` and an `Interaction` it owns; a
  module reaches only the root and the elements it created; a press and a release of the left
  button over one button is a click; a focus change is a blur and a focus.
- **CyberdyneKit**: `UI.swift` — `UIElement`, `UILayout`, `UIStyle`, `UIEvent`, `UI` — and
  `UIBuilder.swift`, a result-builder layer (`Panel`, `Label`, `Image`, `ProgressBar`, `Button`,
  modifiers, `UITree`) mounted once with `Behaviour.mountUI` and updated by handle. No SwiftUI.
  `Behaviour.onUIEvent`, and a button's `action` closure, are reached through `ui_event`.
- **The sample**: `samples/13-rts-api` gains the HUD of `samples/13-rts-selection` written in Swift
  (`game/Hud.swift`) plus a Build button; the commander mounts it, writes it from the game every
  frame, builds a worker when the button is clicked, and asks `UI.hitTest` whether a click was the
  HUD's. The host lays the store out, routes the pointer, sets `OVER_UI`, and delivers events;
  `--no-ui` is the control.
- **Tests**: `unit.abi` (`test_game_ui.cpp`, the 1.6 layouts and table shape), the generated Swift
  layout tests, `integration.game_backend_ui`, CyberdyneKit's `UITests`, an extended
  `integration.rts_api_sample` with the `--no-ui` control, and `render.rts_api_hud`: the Swift HUD
  flattens to the C++ HUD's primitive stream and draws the same bytes on a device, and the Swift
  HUD with its button against a committed golden image.

## Scope

Not built: an interface asset or `.cyss` sheet a module loads (the style is set per element),
keyboard and gamepad navigation from a module (focus is set, not navigated), text input fields,
image upload through the ABI (the embedder uploads atlas pages), world-space documents, and any
widget beyond the five kinds. The sample host is headless; the HUD is photographed by
`render.rts_api_hud`, not drawn by the sample.
