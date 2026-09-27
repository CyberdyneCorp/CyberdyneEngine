## MODIFIED Requirements

### Requirement: Exposure
The engine SHALL support **manual exposure** (EV, or aperture / shutter / ISO) and
**auto-exposure** driven by a luminance histogram computed by compute shaders over the scene
colour.

Auto-exposure SHALL support: a metering mask, histogram percentile-based target selection to
reject outliers, minimum and maximum EV clamps, separate adaptation speeds for brightening and
darkening, and an exposure compensation curve keyed on measured luminance.

Exposure SHALL be applied as a scalar multiply before tonemapping and SHALL also be published to
shaders so emissive values can be expressed in physical units.

#### Scenario: Bright window does not blow out the room
- **WHEN** a small bright region is in view
- **THEN** percentile-based metering SHALL reject the outlier and expose for the room

#### Scenario: Adaptation speed
- **WHEN** the camera moves from dark to bright
- **THEN** exposure SHALL adapt at the configured rate, clamped to the EV range

#### Scenario: Manual exposure is a physical multiply
- **WHEN** a manual EV100 is raised by one
- **THEN** every unclipped pixel of the exposed frame SHALL halve

#### Scenario: Metering is the host's arithmetic on the device
- **WHEN** the histogram and the adaptation run on the device
- **THEN** their histogram SHALL equal the host binning of the same pixels, and their target and
  adapted EV SHALL be the host's percentile metering and adaptation of that histogram

### Requirement: Colour grading
Colour grading SHALL support: white balance (temperature and tint), per-channel lift/gamma/gain,
shadows/midtones/highlights colour wheels with range boundaries, saturation, contrast, hue shift,
channel mixer, and a **3D LUT** (`.cube`) applied in log space.

Grading parameters SHALL be bakeable into a single 3D LUT at build time so the runtime cost is one
texture lookup.

#### Scenario: Grade baked to a LUT
- **WHEN** grading parameters are static
- **THEN** they SHALL be baked into a 3D LUT, and the runtime SHALL apply only that lookup

#### Scenario: Log-space LUT
- **WHEN** a LUT is applied
- **THEN** the input SHALL be converted to a log encoding first, so the LUT has adequate
  precision in shadows

#### Scenario: A neutral grade changes nothing
- **WHEN** the grade's parameters are neutral and any LUT is the identity
- **THEN** the lookup SHALL NOT be applied, and the frame SHALL be the ungraded frame byte for byte

#### Scenario: Display-referred lookup
- **WHEN** the LUT is applied at step 12
- **THEN** it SHALL be indexed by a log encoding of display-referred colour, and a `.cube` SHALL be
  read in its own input encoding
