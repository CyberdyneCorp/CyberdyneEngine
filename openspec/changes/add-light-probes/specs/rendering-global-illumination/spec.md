## ADDED Requirements

### Requirement: Irradiance volumes light the forward frame's ambient term
When a frame is given an irradiance volume, the forward pass SHALL take each surface's ambient
radiance from the volume — the visibility-weighted trilinear blend of the eight surrounding probes'
SH irradiance toward the surface normal — in place of the flat sky ambient, and ambient occlusion
SHALL multiply the result as it multiplied the flat term. Outside the volume the flat ambient SHALL
return, blended over one probe spacing at its boundary. A frame given no volume SHALL shade exactly
as it did before volumes existed.

A volume SHALL be captured from the scene and the sky through the illumination seams
(`SceneTracer`, `RadianceLookup`, `SkyTerm`) and SHALL follow a stated update policy: a full
capture, region invalidation, and a per-update probe budget.

#### Scenario: A coloured wall bleeds onto a nearby white surface
- **WHEN** a white surface that receives no direct light stands beside a sunlit red wall
- **THEN** its ambient term SHALL be measurably redder than the same surface's far from the wall
- **AND** with no volume the two SHALL be the same colour

#### Scenario: An unlit wall is lit by bounce
- **WHEN** a wall no light reaches faces a sunlit floor
- **THEN** its ambient term SHALL vary over it and be brightest where the floor fills its view
- **AND** with no volume it SHALL be uniform

#### Scenario: No volume is the frame as it was
- **WHEN** the volume is uploaded and bound but the frame is not told to use it
- **THEN** the frame SHALL be byte-identical to a frame with no volume, drawn by the frame shader as
  it was before volumes existed

#### Scenario: Probes agree with a reference integration
- **WHEN** a probe's irradiance is compared with a numerical integration of the same scene's
  radiance over the sphere
- **THEN** it SHALL agree with that integration's SH L1 projection within the capture's sampling
  tolerance

#### Scenario: A light change is picked up within a bounded number of updates
- **WHEN** a light changes under the amortised policy with no invalidation
- **THEN** every probe SHALL be re-captured within `ceil(probes / probes_per_update)` updates
