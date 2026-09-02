## ADDED Requirements

### Requirement: Light channels
Lights SHALL declare a **channel mask**, and renderables SHALL declare the channels they receive,
so a light can be restricted to a subset of the world — characters only, a specific interior,
VFX only — for artistic control.

Channel resolution SHALL be a compact bitfield test during light assignment, not a per-object
light loop, so channels cost nothing at shading time.

Channels SHALL be a filter on assignment, and SHALL NOT create separate lighting passes.

#### Scenario: Character-only key light
- **WHEN** a light is restricted to the character channel
- **THEN** it SHALL illuminate characters and SHALL not be assigned to world geometry

#### Scenario: Channels do not cost per pixel
- **WHEN** channels are in use
- **THEN** the cost SHALL be in cluster assignment, not in a per-pixel loop over lights

### Requirement: Stochastic many-light direct lighting
The engine SHALL define a **stochastic direct lighting** path for scenes with very many shadowed
lights, in which each pixel samples a small number of lights chosen by importance rather than
evaluating every light in its cluster.

The path SHALL be: candidate generation from the GPU light set, **reservoir sampling** with
temporal and spatial reuse, ray-traced or shadow-map visibility for the selected samples, and
denoising through `denoising`.

Clustered lighting SHALL remain the **default shipping path**. The stochastic path SHALL be a
profile and scene decision, and content SHALL render correctly under either.

This path SHALL NOT be enabled without denoising, since its output is a noisy estimate; that
dependency SHALL be enforced by configuration validation rather than discovered visually.

Direct and indirect illumination SHALL remain separate solvers. Many-light direct lighting SHALL
NOT be implemented inside the GI system, so the two do not develop divergent lighting models.

#### Scenario: Thousands of shadowed lights
- **WHEN** a scene contains many thousands of shadowed area lights
- **THEN** the stochastic path SHALL evaluate a bounded number of samples per pixel rather than
  iterating the cluster's full light list

#### Scenario: Denoiser is required
- **WHEN** the stochastic path is enabled without denoising
- **THEN** configuration validation SHALL reject it with a diagnostic

#### Scenario: Same content, either path
- **WHEN** a scene is rendered with clustered lighting and with the stochastic path
- **THEN** both SHALL produce correct lighting, differing in cost, noise characteristics, and the
  number of shadowed lights that remain affordable

## MODIFIED Requirements

### Requirement: Light culling and limits
Under the **clustered** path, the number of lights affecting a single cluster SHALL be bounded, and
the number of shadowed lights per view SHALL be bounded by the shadow atlas budget.

Lights exceeding budgets SHALL be dropped deterministically by importance (screen coverage ×
intensity), and the shortfall SHALL be reported.

Under the **stochastic many-light** path, the per-cluster bound SHALL NOT apply: lights are
importance-sampled rather than iterated, and the bound becomes the sample count per pixel and the
visibility ray budget.

The active path SHALL be reported alongside light statistics, since a light limit that does not
apply is more confusing than one that does.

#### Scenario: Deterministic dropping
- **WHEN** the same scene is rendered twice with too many lights under the clustered path
- **THEN** the same lights SHALL be dropped both times, so the result does not flicker between
  frames

#### Scenario: The limit that applies is the one reported
- **WHEN** the stochastic path is active
- **THEN** statistics SHALL report sample counts and ray budgets rather than a per-cluster light
  limit that is not in force
