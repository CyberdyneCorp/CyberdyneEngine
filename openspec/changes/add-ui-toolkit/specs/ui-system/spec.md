## ADDED Requirements

### Requirement: A widget toolkit on the element store
The engine SHALL provide a widget toolkit whose widgets are elements of the UI element store, laid
out, styled and flattened by the same paths as every other element, each part of a widget styled
with its owning widget's states. It SHALL provide at least containers (anchored, flex row and
column, grid, stack, scroll view), label, rich text, image, button, toggle, checkbox, radio group,
slider, progress bar, single-line text field, drop-down, virtualised list view, tabs, tooltip and
modal dialog, and SHALL raise clicked, changed, submitted, focus and opened/closed events for the
player's actions and none for the game's own writes.

#### Scenario: A large list costs what a small one does
- **WHEN** a list view holding ten thousand items scrolls to a new position every frame
- **THEN** it SHALL realise the same rows as a list of a hundred, and its mean frame SHALL stay
  inside a tenth of a 60 Hz frame

#### Scenario: A game's write raises no event
- **WHEN** the game sets a slider's value
- **THEN** no changed event SHALL be raised, while the player's drag raises one

### Requirement: UI documents are cooked and reloaded in place
A `.cyui` document SHALL instantiate other documents with parameters, name its style sheets and its
bindings, and write back byte for byte. Reloading a changed document SHALL keep every widget it can
match with its state and focus, and a document that does not parse SHALL leave the interface as it
was and report the line of its mistake. The build graph SHALL cook a document with everything it
reaches into one bundle, recording each reached file as a dependency.

#### Scenario: A saved document reloads keeping the player's state
- **WHEN** a document on disk is saved with a changed label while the player has moved a slider
- **THEN** the label SHALL change and the slider SHALL keep the player's value and focus

#### Scenario: A shared sheet re-cooks its documents
- **WHEN** a style sheet used by a document's component is saved
- **THEN** the document's cook node SHALL run again, and a node that reads none of it SHALL stay
  cached

### Requirement: Interface actions come from the input system
The `UI.*` actions SHALL be declared in the input server with default keyboard and gamepad bindings,
and while the toolkit holds the input the input user's focus layer SHALL be the interface's, so
that gameplay actions bound to the same controls are not observed.

#### Scenario: A dialog suppresses the game's key
- **WHEN** a modal dialog is open and the player presses a key bound to both UI.Accept and a
  gameplay action
- **THEN** the button SHALL be activated and the gameplay action SHALL not be observed, and the
  gameplay action SHALL be observed again once the dialog closes

### Requirement: Opacity groups and an HDR composite are drawn on the device
An element marked as an opacity group SHALL be drawn offscreen and composited once at its opacity,
and a frame with no group SHALL declare no pass it did not declare before. A document drawn by the
HDR stage SHALL be composited on the linear scene colour before bloom, at the frame's exposure, with
emissive elements brighter than white.

#### Scenario: Overlap at half opacity
- **WHEN** a group with two overlapping opaque children is drawn at 50 % opacity
- **THEN** the overlap SHALL be the colour of either child alone

#### Scenario: An emissive panel blooms
- **WHEN** a panel with an intensity above one is drawn by the HDR stage
- **THEN** the bloom around it SHALL be brighter than with the same panel drawn after the tone curve

### Requirement: Scripting modules reach the widget toolkit
Through the C ABI a module SHALL load documents and style sheets, create and find widgets, read and
write their text, value, selection and items, open dialogs, start animations and deliver navigation
actions, and SHALL receive the widgets' events through its behaviours. A widget the embedder made
SHALL not be reachable by a module, and with no toolkit attached every toolkit entry SHALL answer
UNAVAILABLE.

#### Scenario: A document's slider reports to its owner
- **WHEN** a module loads a document for an entity and the player moves its slider with a gamepad
- **THEN** the entity's behaviours SHALL receive a changed event carrying the new value
