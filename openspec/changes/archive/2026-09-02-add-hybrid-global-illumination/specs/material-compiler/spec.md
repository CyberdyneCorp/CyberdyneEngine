## ADDED Requirements

### Requirement: Secondary material programs
A material SHALL compile to more than one program, so that illumination queries do not pay the
cost of camera-visible shading:

| Program | Used for | Content |
|---|---|---|
| `Primary` | Camera-visible shading | The full graph |
| `Secondary` | Filling the surface cache and answering illumination queries | Reduced texture samples, microdetail removed, secondary closures dropped where their contribution to outgoing radiance is small |
| `FarField` | Distant illumination | Constants and averaged values |

Derivation SHALL be **automatic**, by removing inputs and closures whose contribution to outgoing
radiance falls below a threshold — detail normals, parallax, procedural microdetail, and thin
secondary lobes.

Derivation SHALL be **overridable per material**, because automatic derivation can be wrong: a
material whose base reflectance is produced by a node the compiler treats as microdetail would
bleed the wrong colour into the scene, and an author must be able to correct that.

The compiler SHALL report, per material, the cost of each program and the difference in average
albedo and emission between them, so an incorrect derivation is visible rather than latent.

Secondary programs SHALL count as a permutation axis against the permutation budget.

#### Scenario: A secondary hit is cheap
- **WHEN** the surface cache is filled for a forty-node material
- **THEN** the secondary program SHALL be used, with substantially fewer texture samples than the
  primary program

#### Scenario: Wrong derivation is visible
- **WHEN** automatic derivation changes a material's average albedo significantly
- **THEN** the cook report SHALL flag it, so an author can override the derivation

#### Scenario: Author override
- **WHEN** an author marks a node as contributing to base reflectance
- **THEN** it SHALL be retained in the secondary program regardless of the automatic heuristic

#### Scenario: Far field is constant
- **WHEN** a material is used kilometres from the camera
- **THEN** its far-field program SHALL supply averaged constants rather than sampling textures
