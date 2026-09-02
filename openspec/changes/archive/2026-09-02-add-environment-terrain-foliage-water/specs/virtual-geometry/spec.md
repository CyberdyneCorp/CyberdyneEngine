## MODIFIED Requirements

### Requirement: Deformation classes and growth path
Assets SHALL declare a **deformation class**, and the system SHALL support them incrementally:

| Class | Support |
|---|---|
| `Static` | Required |
| `RigidInstanced` | Required |
| `Terrain` | Supported: produced by `terrain` as a geometry source |
| `Destructible` | Deferred |
| `Skinned` | Deferred |

**Terrain** produces virtual geometry clusters from its tile representation like any other geometry
source, with no parallel streaming or rendering path. Its clusters SHALL be watertight across tile
and detail boundaries.

Deferred classes SHALL have their architectural seams reserved:

- **Destructible** — fragments pre-cooked with cluster mappings and activated as subsets, never
  rebuilt at runtime
- **Skinned** — clusters carrying bone influence sets, deformed from the **GPU pose world**
  (see `animation-and-skinning`) so only visible clusters are deformed, integrated with animation
  LOD

A change adding a deferred class SHALL go through the OpenSpec flow.

#### Scenario: Terrain is one representation
- **WHEN** terrain is rendered
- **THEN** it SHALL produce virtual geometry rather than requiring a duplicate representation
  streamed alongside it

#### Scenario: Destruction does not rebuild at runtime
- **WHEN** an object is destroyed
- **THEN** pre-cooked fragment subsets SHALL be activated, with no runtime hierarchy construction

#### Scenario: A change would close a seam
- **WHEN** a proposal would make the cluster format unable to carry bone influences
- **THEN** it SHALL be flagged against this requirement
