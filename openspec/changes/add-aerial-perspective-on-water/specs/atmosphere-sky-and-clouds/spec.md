# Spec Delta

## MODIFIED Requirements

### Requirement: Aerial perspective
Distance attenuation SHALL be produced by the **atmosphere model** — scattering and transmittance
over distance — and applied to opaque shading and to volumetric media consistently.

Ad-hoc distance fog with independently tuned parameters SHALL NOT be the engine's model of distance,
though a stylised override SHALL remain available for projects that want one.

Aerial perspective SHALL be correct at large scale, so that terrain kilometres away is attenuated
consistently with the sky above it.

A surface SHALL be attenuated by the transmittance between it and the eye and SHALL receive the
light the air scatters into that path, both evaluated from the same atmosphere parameters and the
same precomputed tables as the sky drawn behind it, and expressed in the same radiance units as that
sky. A surface close to the eye SHALL be left as it was lit, to within a bound, rather than
receiving the haze of the nearest table slice.

A surface whose shading composes pictures that were themselves drawn through the air — a
reflection, a refraction — SHALL receive the air in front of it exactly once: what such a picture
already carries SHALL NOT be attenuated or added to again, and what lies beyond the surface in a
medium other than the air SHALL NOT be hazed as though it were air.

#### Scenario: Distance is not separately tuned
- **WHEN** distant terrain is rendered
- **THEN** its attenuation SHALL come from the atmosphere, consistent with the sky

#### Scenario: Stylisation is explicit
- **WHEN** a project wants non-physical distance falloff
- **THEN** it SHALL be a declared override rather than a divergence between two models

#### Scenario: The horizon meets the sky
- **WHEN** geometry far enough away that the air decides its colour is drawn against the sky at the
  horizon
- **THEN** the geometry's colour at its edge SHALL match the sky's colour beside it within a stated
  tolerance

#### Scenario: A changed atmosphere changes both
- **WHEN** the atmosphere's composition or the sun's elevation changes
- **THEN** the sky and distant geometry SHALL change together, and SHALL still meet at the horizon

#### Scenario: Near geometry is left alone
- **WHEN** a surface a few metres from the eye is shaded with aerial perspective on
- **THEN** its colour SHALL differ from the same surface with aerial perspective off by no more
  than a stated bound

#### Scenario: Off is the frame before
- **WHEN** aerial perspective is disabled
- **THEN** the frame SHALL be identical to the frame drawn without it

#### Scenario: Water is hazed like the land beside it
- **WHEN** a water surface and land at the same distance from the eye are drawn with aerial
  perspective on
- **THEN** both SHALL receive the same in-scattering within a stated tolerance, so the shoreline
  shows no step in haze, near and far

#### Scenario: A reflection is not hazed twice
- **WHEN** a surface's reflection or refraction is read from a picture already drawn through the air
- **THEN** the surface's colour SHALL equal the air in front of it applied once to its own light,
  with the picture's own air left as it was drawn, within a stated tolerance
