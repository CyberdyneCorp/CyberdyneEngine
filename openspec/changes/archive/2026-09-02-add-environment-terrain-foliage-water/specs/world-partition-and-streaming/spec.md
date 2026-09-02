## MODIFIED Requirements

### Requirement: Subsystem integration contracts
The world SHALL define the contract by which each subsystem consumes cell lifecycle:

| Subsystem | Contract |
|---|---|
| Rendering | Cell activation publishes instances into the GPU scene; virtual geometry streams detail separately |
| Physics | Resident cells may preload collision; activation registers bodies in bulk |
| Navigation | Cell events drive tile residency; navigation owns its own tile layout, which need not match cells |
| Illumination | Cells carry GI payloads ingested by the GI scene, and evicted on unload |
| Audio | Cells carry ambient zones, reverb metadata, and acoustic geometry |
| Networking | Replication cells derive from the world partition |
| AI | Cell events drive representation tier changes |
| Environment fields | Cells carry field tiles, ingested on residency and evicted on unload (see `environment-fields`) |
| Terrain | Cells carry terrain tile data, collision, and navigation contribution as separate channels (see `terrain`) |
| Foliage | Cells carry foliage clusters and placement rule bindings; instances publish into the GPU scene (see `foliage`) |
| Water | Cells carry water body **segments**; the body's identity and network are global while its runtime data is segmented (see `water`) |

Subsystems SHALL own their internal granularity. The world SHALL NOT impose its cell boundaries on
a subsystem whose optimal partition differs.

A subsystem whose logical object spans many cells — a river, an ocean, a terrain — SHALL be
represented as one logical entity with segmented runtime data, rather than as unrelated per-cell
objects.

#### Scenario: Navigation keeps its own tiling
- **WHEN** a cell activates
- **THEN** navigation SHALL update its own tiles, which need not align with cell boundaries

#### Scenario: Two scales of streaming
- **WHEN** a region streams in
- **THEN** the world SHALL establish which objects exist, and virtual geometry and texture
  streaming SHALL determine their detail independently

#### Scenario: A river is one thing
- **WHEN** a river crosses hundreds of cells
- **THEN** it SHALL be one water body with segmented runtime data, not hundreds of independent
  water objects
