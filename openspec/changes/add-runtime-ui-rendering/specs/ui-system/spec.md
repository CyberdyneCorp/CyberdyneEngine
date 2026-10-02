## ADDED Requirements

### Requirement: The flattened stream is drawn on the device
The flattened primitive stream SHALL be drawn by one pass at the frame's interface stage, after
tonemapping and after selection outlines, over the colour the post chain ended in. Each batch SHALL be
one indirect draw with its clip rectangle as a scissor, and rectangles, rounded rectangles, borders,
images and glyphs SHALL be drawn by one shader selected by the primitive's material. Colours SHALL be
premultiplied with the element's inherited opacity folded in, and blended over the frame. Until real
fonts are imported, the engine SHALL carry a built-in bitmap font so that text reaches the screen
through the text server's layout and glyph atlas.

#### Scenario: No interface leaves the frame untouched
- **WHEN** no interface is attached, or one is attached whose document draws nothing
- **THEN** the frame SHALL be byte-identical to the frame before the interface pass existed

#### Scenario: A colour lands as asked
- **WHEN** an opaque panel or glyph covers whole pixels
- **THEN** those pixels SHALL hold its colour byte for byte, and a later sibling SHALL cover an
  earlier one

#### Scenario: Nested scroll views clip to their intersection on screen
- **WHEN** a scroll view inside a scroll view holds content larger than both
- **THEN** exactly the pixels of the intersection of the two clips SHALL be drawn, and every other
  pixel SHALL be the frame's own

#### Scenario: One draw per batch
- **WHEN** a document flattens to N batches none of which is clipped away
- **THEN** the pass SHALL record N indirect draws

### Requirement: A developer console ships on CyberUI
The engine SHALL ship a developer console built on CyberUI's element store and drawn by the interface
pass: an input line, registered commands with replies, and a scrollback whose rows are virtualised so
that the number of elements does not grow with the number of lines printed.

#### Scenario: The scrollback does not grow the store
- **WHEN** a thousand lines are printed into a console that shows five
- **THEN** the store SHALL hold the same five row elements, and printing SHALL repaint without a
  relayout
