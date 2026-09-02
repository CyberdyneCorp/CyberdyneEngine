## MODIFIED Requirements

### Requirement: The GI scene
The engine SHALL maintain a **GI scene**: the representation of the world used to answer
illumination queries, derived from the GPU scene and maintained incrementally.

The GI scene SHALL be **deliberately coarser than primary visibility**, with its own error
targets:

| Query | Target error |
|---|---|
| Primary visibility | Sub-pixel geometric error |
| Near-field illumination | Centimetres of world-space error |
| Far-field illumination | Metres of world-space error |

Where an asset has a virtual geometry hierarchy, the GI scene SHALL select a coarser level from
that hierarchy rather than building a separate simplification.

The GI scene SHALL be **cell-scoped**: illumination payloads SHALL be cooked as a cell channel and
ingested on cell residency, and evicted on cell unload, so no monolithic global structure exists.
Cell lifecycle SHALL come from `world-partition-and-streaming`.

Ingestion and eviction SHALL be incremental and SHALL invalidate only the affected region, so
streaming does not cause a global illumination rebuild.

#### Scenario: Illumination does not pay for pixel-accurate geometry
- **WHEN** a ray is traced for indirect diffuse
- **THEN** it SHALL intersect a representation at the illumination error target, not the primary
  visibility representation

#### Scenario: One hierarchy, two error targets
- **WHEN** an asset has a virtual geometry hierarchy
- **THEN** the GI scene SHALL request a coarser level from it, rather than cooking a second
  simplified mesh

#### Scenario: Region unloads
- **WHEN** a world cell is unloaded
- **THEN** its GI payloads SHALL be evicted, the affected region invalidated, and queries into it
  SHALL fall back to the far-field representation

#### Scenario: Streaming does not rebuild illumination
- **WHEN** cells stream in and out as the camera moves
- **THEN** GI scene updates SHALL be incremental and local, not a global rebuild
