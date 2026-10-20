## ADDED Requirements

### Requirement: A card's direct light is its footprint's
A surface card SHALL be shaded with the mean of the direct light over the surface it stands for, not
with the light at one point of it: each light's unshadowed contribution at the card times the share
of the card's footprint that light reaches past the occluder, found by shadow tests at a stated
lattice of sample points over the footprint and refined to a finer lattice wherever the coarse
samples disagree. With no occluder the card's direct light SHALL be the point evaluation's exactly.
The device card shading SHALL transcribe the host's and agree with it within the stated bound.

#### Scenario: The sunlit patch carries the window's light
- **WHEN** a closed room is lit by the sun through a window a few cards across, and its cards are
  shaded
- **THEN** the light the floor's cards carry SHALL be the sunlight falling through the window within
  a stated share, and the light they throw on the ceiling and the far wall in one bounce SHALL be the
  path tracer's within a stated relative error

#### Scenario: A card a shadow edge crosses takes the lit share
- **WHEN** a shadow's edge crosses a card
- **THEN** its direct light SHALL be the unshadowed light times the share of its footprint the light
  reaches, on the host and on the device alike, and a card wholly in shadow SHALL receive none of
  that light

### Requirement: An interior within budget at 1080p
The default dynamic GI — the screen probes and their probe-space denoiser, through the hardware tier
where the device has it — SHALL light a furnished interior lit through windows at 1920x1080 within
a stated GPU budget, every stage counted, with its error against the path tracer stated beside the
path tracer's own noise, and with no worse error, leak, still-view motion or settle time in the GI
test room than before. The device card lookup a traced hit resolves through SHALL answer with the
card the host lookup answers with.

#### Scenario: The lookup reads one cell and finds the host's card
- **WHEN** hits about the cards of a room are resolved through the device lookup structure
- **THEN** each SHALL name the card `CardSnapshot::find` names, and none SHALL be missed because the
  card lies in a neighbouring cell

#### Scenario: A game-like interior beside the path tracer
- **WHEN** a hall lit by the sun through two windows and by an emissive lamp — coloured walls and
  props in the engine's own materials — is captured with the comparison at 1920x1080
- **THEN** the frame without the GI, the frame with it at its default settings, the path tracer's
  frame and the error map SHALL be published, each caption stating the GI's GPU milliseconds and
  the error against the path tracer, and a short film of the hall lit by the GI under a moving
  camera SHALL be published beside them
