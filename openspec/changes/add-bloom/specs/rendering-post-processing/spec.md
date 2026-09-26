## MODIFIED Requirements

### Requirement: Bloom
Bloom SHALL be produced by a progressive downsample and upsample chain with a **soft threshold**
knee, physically-motivated by default (energy scattered from bright sources) with an artistic
intensity control.

The implementation SHALL use a Karis average on the first downsample to suppress fireflies, and a
tent filter on upsample to avoid blocky artifacts.

An optional **lens dirt** and **anamorphic stretch** SHALL be supported.

A disabled bloom SHALL be absent from the frame: its passes SHALL not be declared and its targets
SHALL not be allocated.

#### Scenario: Firefly suppression
- **WHEN** a single very bright pixel appears
- **THEN** the Karis average SHALL prevent it from producing a flickering bloom star

#### Scenario: Energy-conserving default
- **WHEN** bloom is enabled with default settings
- **THEN** total image energy SHALL be approximately preserved, redistributed rather than added

#### Scenario: A scene below the threshold is unchanged
- **WHEN** bloom runs over a frame with no pixel above the threshold knee
- **THEN** the frame SHALL be returned unchanged

#### Scenario: Bloom off is the frame without bloom
- **WHEN** bloom is disabled
- **THEN** no bloom pass SHALL be declared and the post-process SHALL read the colour it read before
  bloom existed
