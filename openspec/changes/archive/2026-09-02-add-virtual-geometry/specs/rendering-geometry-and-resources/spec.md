## MODIFIED Requirements

### Requirement: Mesh LOD
A mesh asset SHALL carry a **traditional representation**, a **virtual geometry representation**
(see `virtual-geometry`), or both.

The traditional representation SHALL support LOD chains generated at import using a simplification
library, with per-level screen coverage thresholds, and optional **shadow proxy** and **collision
proxy** levels. Simplification SHALL preserve UV seams, normals within a tolerance, and material
boundaries.

Where an asset has a virtual representation, its traditional representation serves as the
**fallback**: used on devices without the required capabilities, for ray tracing acceleration
structures, for physics proxy generation, and for editor tooling. It SHALL still be produced.

Authored LOD chains SHALL remain supported for the traditional path; they SHALL NOT be required for
assets using virtual geometry.

#### Scenario: LOD generation at import
- **WHEN** a mesh is imported with LOD enabled
- **THEN** a chain SHALL be generated to the configured triangle reduction targets, with UV seams
  preserved

#### Scenario: Manual LODs
- **WHEN** an artist supplies hand-authored LOD meshes
- **THEN** they SHALL be used instead of generated ones, with the same threshold mechanism

#### Scenario: Virtual asset still has a fallback
- **WHEN** an asset is cooked with virtual geometry enabled
- **THEN** a traditional representation SHALL also be produced for fallback, ray tracing, and
  tooling use

#### Scenario: No manual LODs required
- **WHEN** an asset uses virtual geometry
- **THEN** no LOD chain SHALL need to be authored for its primary rendering path
