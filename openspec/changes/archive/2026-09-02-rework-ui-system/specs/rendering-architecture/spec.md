## MODIFIED Requirements

### Requirement: GPU scene
The renderer SHALL maintain a **GPU scene**: the authoritative GPU-side representation of
renderable instances, from which GPU-driven culling, LOD selection, and indirect drawing are
performed.

The GPU scene SHALL hold per instance at minimum: a transform and its previous-frame value,
bounds, a mesh reference, a material reference, an LOD chain reference, a layer mask, and instance
flags.

Instances SHALL be publishable into the GPU scene from multiple producers:

- the **extract** stage, from ECS entities with renderable components
- **instanced mesh** components, from their transform buffers
- the **VFX system**, from mesh particles (see `vfx-system`)
- the **UI system**, from world-space and surface-space UI documents (see `ui-system`)

All producers SHALL use the same representation, so downstream culling, LOD, sorting, and drawing
require no knowledge of an instance's origin.

Instance publication SHALL be possible entirely GPU-side, without CPU round trips, for producers
whose data already lives on the GPU.

#### Scenario: One representation, many producers
- **WHEN** mesh particles, instanced meshes, world-space UI, and ordinary entities are all visible
- **THEN** they SHALL occupy the same GPU scene representation and be culled and drawn by the same
  passes

#### Scenario: GPU-side publication
- **WHEN** a producer's instance data is computed on the GPU
- **THEN** it SHALL publish into the GPU scene from a compute shader, with no readback and no CPU
  submission per instance

#### Scenario: Producer removed
- **WHEN** an effect, entity, or UI document is destroyed
- **THEN** its instances SHALL be removed from the GPU scene without requiring a full rebuild
