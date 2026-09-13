## ADDED Requirements

### Requirement: The sky term is the atmosphere's, and the seam is a fact about the link graph
`rendering-global-illumination` already defines what illumination **consumes** from the sky: a
filtered radiance map, irradiance, sun and celestial transmittance, cloud shadowing and aerial
perspective. `atmosphere-sky-and-clouds` already defines the sky that produces them. **What neither
states is that the two are joined**, and on a tree where they are not, every scenario in "Sky and
atmosphere" is satisfied by a placeholder gradient with nobody able to tell.

The engine SHALL construct its sky illumination term from the physical atmosphere, at a composition
point reachable from shipping code rather than only from a test, and the join SHALL be checkable as a
property of the link graph rather than asserted in prose — the way one-producer-per-field is a
refusal at registration rather than a convention.

A two-colour or analytic gradient MAY remain as a declared fallback for a configuration with no
atmosphere, and where it is used the engine SHALL report that the term is the fallback rather than
the atmosphere, so that a picture lit by the placeholder is distinguishable from one lit by the sky.

#### Scenario: Nothing outside a test constructs the term
- **WHEN** no shipping code path constructs the illumination sky term from the atmosphere
- **THEN** the check SHALL fail naming the modules that do not reach each other, rather than the row
  being reported as satisfied by the presence of both

#### Scenario: The placeholder is reported as a placeholder
- **WHEN** illumination runs with no atmosphere present and falls back to the analytic gradient
- **THEN** the frame's diagnostics SHALL report the fallback, so a published picture's lighting
  provenance is readable rather than inferred

#### Scenario: A moving sun is incremental and bounded
- **WHEN** the sun rotates continuously across a day
- **THEN** the illumination the sky feeds SHALL be updated incrementally and within the illumination
  budget, and the invalidation this triggers SHALL be measured against a full recomputation rather
  than assumed smaller
