## MODIFIED Requirements

### Requirement: Fallback and platform paths
Every asset SHALL have a **fallback mesh** representation, used for: devices without the required
GPU capabilities, ray tracing acceleration structures, physics proxy generation, editor tooling,
and any path where virtual geometry is unavailable.

The renderer SHALL select a **geometry path** per device: virtual geometry, mesh-shader-accelerated,
or traditional indexed rendering, based on capability queries rather than device identity.

Content SHALL NOT depend on virtual geometry being available; a project SHALL be shippable on a
device restricted to the fallback path, with reduced detail rather than missing content.

Ray tracing SHALL initially use the fallback representation. Native ray tracing against virtual
geometry is recorded as deferred.

**Illumination representations** SHALL be drawn from the cluster hierarchy rather than from the
fallback mesh where the hierarchy is available: the GI scene (see `rendering-global-illumination`)
selects a level meeting an illumination error target measured in world-space distance, which is
far coarser than the primary visibility target. One hierarchy SHALL serve both, and no separate
simplified mesh SHALL be cooked for illumination.

#### Scenario: Unsupported device
- **WHEN** a device lacks the required capabilities
- **THEN** assets SHALL render through the fallback path, with the reduced detail reported

#### Scenario: Ray tracing uses the fallback
- **WHEN** acceleration structures are built
- **THEN** the fallback representation SHALL be used, and the resulting detail difference SHALL be
  documented

#### Scenario: Illumination uses a coarser hierarchy level
- **WHEN** the GI scene requests a representation for an asset
- **THEN** it SHALL receive a level of the existing cluster hierarchy chosen for a world-space
  error target, not a separately authored or cooked simplification
