## ADDED Requirements

### Requirement: The visual designer previews through the engine
The editor's UI layout editor SHALL edit UI documents as their text form and SHALL show them as the
ENGINE lays them out and draws them: the editor SHALL send the document, every file it reaches and
the preview settings to the runtime, and SHALL show the picture and the laid-out rectangles the
runtime answers, never a layout of its own. The palette SHALL offer exactly the widget kinds the
engine's toolkit declares, and the editor SHALL refuse to open until the engine has declared them.
The preview SHALL be settable to a resolution, a scale mode, a DPI scale, a theme, and a state of
one widget (hovered, pressed, focused, disabled, open). Placing, reparenting, laying out (anchors,
offsets, size and margins), naming, classing, binding and styling a widget SHALL each be one
undoable transaction and one agent tool of the same name. A save SHALL write the text the engine's
writer writes for the same document, and a running game that loaded the document by path SHALL
reload it.

#### Scenario: The canvas is the engine's
- **WHEN** a document is opened in the layout editor with a runtime attached
- **THEN** the canvas SHALL show the picture the runtime drew and select a widget by the rectangle
  the runtime laid it out at

#### Scenario: A state and a theme are previewed
- **WHEN** a button is previewed pressed, and the document is previewed in another theme
- **THEN** the runtime SHALL draw the button in its pressed style and the document in that theme's
  colours, and the document's text SHALL not change

#### Scenario: An edit is undone
- **WHEN** a widget is added, laid out and bound, and each edit is undone
- **THEN** the document SHALL return to its text before each edit, and the canvas SHALL be drawn
  again after each

#### Scenario: A save reaches a running game
- **WHEN** a document a running game loaded by path is saved
- **THEN** the game SHALL show the saved document, with its widgets' handles and state kept

#### Scenario: Designer opens what code wrote
- **WHEN** a document the engine's writer wrote is opened in the layout editor and saved unchanged
- **THEN** its text SHALL be unchanged, byte for byte
