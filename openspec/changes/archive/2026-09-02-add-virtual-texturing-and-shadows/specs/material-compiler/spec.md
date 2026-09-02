## MODIFIED Requirements

### Requirement: Secondary material programs
A material SHALL compile to a **family of programs**, so that a consumer does not pay the cost of
camera-visible shading for work that does not need it:

| Program | Used for | Content |
|---|---|---|
| `Primary` | Camera-visible shading | The full graph |
| `Secondary` | Filling the surface cache and answering illumination queries | Reduced texture samples, microdetail removed, secondary closures dropped where their contribution to outgoing radiance is small |
| `FarField` | Distant illumination | Constants and averaged values |
| `Shadow` | Shadow rasterisation | Opacity masking and geometric displacement only, plus transmission where supported |

Derivation SHALL be **automatic**, by removing inputs and closures whose contribution to the
program's purpose falls below a threshold, and **overridable per material**, because automatic
derivation can be wrong: a material whose base reflectance or whose opacity is produced by a node the
compiler treats as microdetail would bleed the wrong colour or cast the wrong silhouette.

The `Shadow` program SHALL additionally support **distance tiers**, so a distant shadow page samples
a cheaper opacity representation than a near one. Opaque materials SHALL produce no fragment work in
the shadow program at all.

Textures sampled by the `Shadow` program SHALL be marked **shadow-critical**, so that a coarse
representation of them is guaranteed resident and shadow rasterisation never waits on texture
residency (see `virtual-shadows` and `residency`).

The compiler SHALL report, per material, the cost of each program and the difference in average
albedo, emission, and opacity coverage between them, so an incorrect derivation is visible rather
than latent.

Each program SHALL count as a permutation axis against the permutation budget.

#### Scenario: A secondary hit is cheap
- **WHEN** the surface cache is filled for a forty-node material
- **THEN** the secondary program SHALL be used, with substantially fewer texture samples than the
  primary program

#### Scenario: Wrong derivation is visible
- **WHEN** automatic derivation changes a material's average albedo or opacity coverage
  significantly
- **THEN** the cook report SHALL flag it, so an author can override the derivation

#### Scenario: Author override
- **WHEN** an author marks a node as contributing to base reflectance or to opacity
- **THEN** it SHALL be retained in the corresponding program regardless of the automatic heuristic

#### Scenario: Far field is constant
- **WHEN** a material is used kilometres from the camera
- **THEN** its far-field program SHALL supply averaged constants rather than sampling textures

#### Scenario: Foliage casts a correct silhouette cheaply
- **WHEN** an alpha-tested foliage material rasterises into a shadow page
- **THEN** the shadow program SHALL sample only the opacity mask and its declared displacement, and
  the silhouette SHALL match the primary program's
