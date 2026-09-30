# rendering-global-illumination Spec Delta

## ADDED Requirements

### Requirement: A lightmap description names the scene's objects and its irradiance volumes
A lightmap bake description SHALL be able to name the scene object each light and instance came
from, an instance that only occludes, the cooked material an object draws with, and irradiance
volumes. The bake SHALL capture each described volume with its own path tracer and write the
captured probes beside the cooked lightmap. A description that names none of these SHALL bake
exactly as before. Outside the build graph, a bake SHALL be keyed by the graph's own derivation over
the description and every file it reads, and a bake whose key is unchanged SHALL NOT run again.

#### Scenario: The written probes are the volume's
- **WHEN** a description with a volume is baked
- **THEN** the probes written SHALL equal, bit for bit, the same volume captured directly over the
  same level

#### Scenario: An old description still reads
- **WHEN** a description with no identities, occluders or volumes is read
- **THEN** its lights SHALL be numbered from one and stationary, its instances numbered from zero
  and receiving, and it SHALL have no volume

### Requirement: A lightmap bake sees a cancel within a bounded amount of work
A lightmap bake SHALL check for a cancel at an interval bounded by the work traced — texels times
samples per texel — as well as by texels, so that a bake with many samples per texel stops soon
after it is cancelled.

#### Scenario: A many-sample bake stops within a second
- **WHEN** a bake at 2048 samples per texel is cancelled while it traces real surface
- **THEN** it SHALL trace no more than one interval of texels after the cancel and stop within a
  second

#### Scenario: An irradiance volume capture stops within a second
- **WHEN** the bake's capture of a 4096-probe, 1024-ray irradiance volume is cancelled part way
  through the volume
- **THEN** it SHALL stop within a second, reading the cancel before every probe, and commit none of
  that volume's probes
