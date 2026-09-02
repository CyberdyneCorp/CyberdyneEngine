## MODIFIED Requirements

### Requirement: Semantic input actions
UI SHALL consume **semantic actions** from `input-and-actions`, never raw device input:
`UI.Accept`, `UI.Cancel`, `UI.NavigateUp`, `UI.NavigateDown`, `UI.NavigateLeft`,
`UI.NavigateRight`, `UI.NextTab`, `UI.PreviousTab`, `UI.Context`, `UI.ScrollUp`, `UI.ScrollDown`.

Interface actions SHALL live in **mapping contexts** pushed onto the input user's context stack, so
that a modal consumes navigation while a background context does not observe it, and so that
gameplay actions bound to the same controls are suppressed while the interface holds focus.

The engine SHALL provide platform-appropriate default bindings for keyboard and mouse, gamepad,
and touch, and SHALL expose the **active control scheme** so UI can display correct button glyphs,
with the hysteresis defined in `input-and-actions` so glyphs do not flicker.

Text entry SHALL use the platform text input path, not interface actions or key interpretation.

Directional navigation SHALL support explicit neighbours and geometric fallback, constrained to
the focused layer's scope.

#### Scenario: Device-agnostic game code
- **WHEN** a button handles `UI.Accept`
- **THEN** it SHALL respond to Enter, left click, gamepad south button, or tap, with no
  device-specific code

#### Scenario: Glyph display follows the device
- **WHEN** the player switches from keyboard to gamepad
- **THEN** prompts SHALL update to the gamepad's glyphs

#### Scenario: Navigation stays in the layer
- **WHEN** directional navigation reaches the edge of a modal
- **THEN** focus SHALL NOT escape to elements beneath it

#### Scenario: A modal suppresses gameplay
- **WHEN** a modal interface layer is open
- **THEN** its context SHALL consume the actions it uses, and gameplay bound to the same controls
  SHALL NOT observe them
