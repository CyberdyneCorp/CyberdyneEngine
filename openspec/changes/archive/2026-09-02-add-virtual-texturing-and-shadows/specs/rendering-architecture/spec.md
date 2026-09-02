## MODIFIED Requirements

### Requirement: GPU scene
The renderer SHALL maintain a **GPU scene**: the authoritative GPU-side representation of
renderable instances, from which GPU-driven culling, LOD selection, and indirect drawing are
performed.

The GPU scene SHALL hold per instance at minimum: a transform and its previous-frame value,
bounds, a mesh reference, a material reference, an LOD chain reference, a layer mask, instance
flags, and a **render importance** value (see `residency`).

Render importance SHALL be published once per instance and consumed by every quality decision —
geometry detail, texture page priority, shadow page resolution and refresh, animation rate, and
illumination quality — so that subsystems do not maintain independent notions of what matters.

Instances SHALL be publishable into the GPU scene from multiple producers:

- the **extract** stage, from ECS entities with renderable components
- **instanced mesh** components, from their transform buffers
- the **VFX system**, from mesh particles (see `vfx-system`)
- the **UI system**, from world-space and surface-space UI documents (see `ui-system`)
- **foliage**, from its instance clusters (see `foliage`)
- **terrain** and **water**, as procedural geometry sources

All producers SHALL use the same representation, so downstream culling, LOD, sorting, and drawing
require no knowledge of an instance's origin.

Instance publication SHALL be possible entirely GPU-side, without CPU round trips, for producers
whose data already lives on the GPU.

The GPU scene SHALL retain per-instance **previous and current bounds**, which shadow invalidation
and motion vectors both consume, so neither derives them independently.

#### Scenario: One representation, many producers
- **WHEN** mesh particles, instanced meshes, world-space UI, foliage, terrain, and ordinary entities
  are all visible
- **THEN** they SHALL occupy the same GPU scene representation and be culled and drawn by the same
  passes

#### Scenario: GPU-side publication
- **WHEN** a producer's instance data is computed on the GPU
- **THEN** it SHALL publish into the GPU scene from a compute shader, with no readback and no CPU
  submission per instance

#### Scenario: Producer removed
- **WHEN** an effect, entity, or UI document is destroyed
- **THEN** its instances SHALL be removed from the GPU scene without requiring a full rebuild

#### Scenario: Importance is declared once
- **WHEN** an instance is marked important
- **THEN** geometry, texture, shadow, animation, and illumination quality SHALL all follow from that
  one value
