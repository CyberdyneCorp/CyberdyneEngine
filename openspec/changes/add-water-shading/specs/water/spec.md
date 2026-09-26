# Spec Delta

## MODIFIED Requirements

### Requirement: Water surface shading
Water SHALL be shaded through a **water surface closure** in the material system (see
`material-compiler` and `rendering-materials-and-shading`), not forced through the opaque
metallic-roughness model.

The closure SHALL account for: reflection, refraction, wavelength-dependent **absorption** and
**scattering** through the water column, surface roughness, normals, and foam coverage.

Colour with depth SHALL follow physically-inspired attenuation over the water column thickness,
so deep water darkens and shifts hue without hand-authored gradients.

Reflections SHALL use the illumination hierarchy (see `rendering-global-illumination`): screen
tracing first, escalating to world or hardware tracing by confidence and roughness, with distant
rough water resolved from cached radiance.

What the surface reflects SHALL be the scene and the sky as they are drawn, weighted by the Fresnel
reflectance of the body's refractive index, so that a reflection and the sky above it cannot
disagree. Where the water column is shallow against a shore, foam coverage SHALL rise with the
column's thinness, and where it is deeper than the body's declared foam band the surface SHALL be
shaded exactly as it would be without shoreline foam.

#### Scenario: Depth changes colour physically
- **WHEN** water deepens away from a shore
- **THEN** its colour SHALL change through absorption over the water column rather than through a
  painted gradient

#### Scenario: Rough water does not trace
- **WHEN** water is rough and distant
- **THEN** its reflection SHALL come from cached radiance rather than dedicated rays

#### Scenario: The sky reflected is the sky drawn
- **WHEN** calm water under an open sky is seen at any angle
- **THEN** its reflected radiance SHALL be the Fresnel reflectance at that angle times the sky's
  radiance in the mirrored direction

#### Scenario: Foam only at the shore
- **WHEN** shoreline foam is enabled over a bed that shelves out of the water
- **THEN** the surface over a column thinner than the foam band SHALL be brighter with foam, and the
  surface over a deeper column SHALL be unchanged

#### Scenario: The frame without water shading is unchanged
- **WHEN** water shading is disabled
- **THEN** the frame SHALL be identical to the frame drawn before water shading existed

### Requirement: Caustics
Caustics SHALL be supported in tiers: a projected animated approximation, a surface-derived
approximation computed from the water surface, and a traced solution where ray tracing is
available.

The tier SHALL be selected by renderer profile and by the GI budget allocation, and the surface
derived tier SHALL be the default for real-time use.

Caustics SHALL apply both underwater and, where appropriate, to surfaces above shallow water.

The surface-derived tier SHALL focus the sun by the curvature of the same wave trains the surface
is displaced by, weakened by the water the light crosses to reach the bed, and SHALL move only the
sun's share of the bed's light.

#### Scenario: Caustics follow the actual surface
- **WHEN** the surface-derived tier is active
- **THEN** caustic patterns SHALL follow the simulated water surface rather than an unrelated
  looping texture

#### Scenario: Caustics move with the water
- **WHEN** the water's clock advances and nothing else changes
- **THEN** the caustic pattern on the bed SHALL move, and a frame without caustics SHALL not change
