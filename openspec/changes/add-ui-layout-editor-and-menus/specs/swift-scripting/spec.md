## ADDED Requirements

### Requirement: Game settings through the native ABI
The native ABI SHALL let a game read and apply its display settings — resolution among the modes
the display offers, a quality preset, an upscaler mode, fullscreen and vertical sync — and read the
modes offered; the host's renderer SHALL act on settings a game applied. The native ABI SHALL let a
game write and read the player's files by a plain name in a per-game user-data directory, a write
replacing the file whole. A game's widget SHALL be able to carry a tooltip. The Swift package SHALL
wrap these entries with the same refusals.

#### Scenario: A resolution not offered is refused
- **WHEN** a game applies a resolution the display does not offer
- **THEN** the call SHALL be refused and the settings in effect SHALL not change

#### Scenario: Applied settings reach the renderer
- **WHEN** a game applies 1920x1080 with an upscaler mode
- **THEN** the host's renderer SHALL draw the next frame at 1920x1080 output, rendered at the
  upscaler's internal extent

#### Scenario: A player's file survives a restart
- **WHEN** a game writes a settings file and a later run reads it
- **THEN** the later run SHALL read the same bytes, and a name that is not a plain file name SHALL
  be refused
