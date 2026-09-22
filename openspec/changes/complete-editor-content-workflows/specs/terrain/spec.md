# terrain Spec Delta

## ADDED Requirements

### Requirement: Heightfield ingestion and editor painting
The asset pipeline SHALL import heightfields with explicit horizontal units, vertical range,
resolution and tile layout into stable terrain source assets. Ambiguous channel, signedness, range
or coordinate metadata SHALL produce a structured diagnostic rather than a guessed terrain.

The terrain editor SHALL author sculpting and material painting as stable, ordered modifiers and
layer strokes. It SHALL evaluate only affected tiles asynchronously and SHALL preserve authored
modifiers when evaluation is cancelled or fails.

#### Scenario: Heightfield metadata is explicit
- **WHEN** an author imports a heightfield whose vertical range is not encoded unambiguously
- **THEN** the importer SHALL request or require an explicit range rather than guessing

#### Scenario: Local sculpt recomputes local tiles
- **WHEN** an author sculpts within one brush footprint
- **THEN** only intersecting terrain tiles and dependent collision/render data SHALL be regenerated

#### Scenario: Failed evaluation preserves authoring
- **WHEN** terrain evaluation fails after a modifier is authored
- **THEN** the modifier SHALL remain editable
- **AND** the last valid rendered terrain SHALL remain visible with a structured diagnostic
