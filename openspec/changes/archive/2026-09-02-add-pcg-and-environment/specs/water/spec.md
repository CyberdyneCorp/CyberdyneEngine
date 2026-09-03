## MODIFIED Requirements

### Requirement: Weather and hydrology seams
Water SHALL consume wind, precipitation, and temperature from **environment fields**, whose producer
is `weather-and-wind`.

Wind SHALL drive the ocean spectrum and lake wave response; precipitation SHALL contribute to
wetness, surface disturbance, and — where a project models it — to flow and level; temperature SHALL
influence evaporation and freezing where a project models them.

Rainfall driving runoff, streams, river flow, flooding, and erosion SHALL be recorded as a
**hydrology seam**: the inputs and outputs are fields, and a hydrology capability would be a field
producer and consumer rather than a change to terrain or water. Hydrology is **not specified**, and
is now the last environmental gap.

Water SHALL remain functional without hydrology: flow and level may be authored or driven by a
project, and rainfall's effect on them is a project decision until that capability exists.

#### Scenario: Weather arrives later without rework
- **WHEN** weather drives the wind field
- **THEN** the ocean spectrum SHALL follow it with no change to water's implementation

#### Scenario: Water works without weather
- **WHEN** no weather system is active
- **THEN** wind and precipitation fields SHALL take authored values and water SHALL function

#### Scenario: The remaining gap is prepared
- **WHEN** hydrology is introduced
- **THEN** it SHALL produce and consume existing fields rather than requiring changes to water or
  terrain
