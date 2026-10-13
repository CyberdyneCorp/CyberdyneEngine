## ADDED Requirements

### Requirement: The radiance cache and the tracing tiers run on a device behind the host seams
The engine SHALL be able to gather the radiance cache's probes and answer its queries on a graphics
device, and SHALL be able to trace the screen, software and hardware tiers there, behind the seams
the host subsystems meet through. The device SHALL gather exactly the probes the host scheduler
selects, SHALL resolve every hit through the surface cache, and SHALL remain checked against the
host implementation, which stays the oracle.

#### Scenario: Device probe irradiance matches the host radiance cache
- **WHEN** the same probes are gathered on the device and on the host over the same scene, through
  the software tier or the hardware tier
- **THEN** each probe's payload, visibility distances and error SHALL agree within a stated bound

#### Scenario: The device query matches the host query
- **WHEN** the device answers `RadianceCache::sample` at a set of points
- **THEN** the radiance and the confidence SHALL agree with the host's within a stated bound

#### Scenario: The hardware and software tiers agree on the device
- **WHEN** one scene is converged on the device once through each world tier
- **THEN** the resolved indirect diffuse SHALL differ by less than the fallback tolerance the host
  states

#### Scenario: A device without ray queries falls back without a visible step
- **WHEN** ray queries are switched off mid-run, or the device was created without them
- **THEN** the software tier SHALL answer, the switch alone SHALL move the image no more than an
  ordinary frame does, and the two causes SHALL produce the same image

#### Scenario: The screen tier's pyramid changes no answer
- **WHEN** a batch is traced through the screen tier with and without the nearest-depth pyramid
- **THEN** every answer SHALL be identical, and over open ground the pyramid SHALL read fewer texels
