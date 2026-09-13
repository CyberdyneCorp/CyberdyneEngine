## MODIFIED Requirements

### Requirement: Mobile pipeline differences
The Mobile pipeline SHALL share the renderer's structure but:

- omit the depth prepass by default (bandwidth over overdraw on tile GPUs)
- use per-object light lists rather than a cluster grid, bounded per object
- run tonemapping as a **subpass** so HDR colour never leaves tile memory
- omit screen-space reflections, screen-space GI, and subsurface scattering
- prefer memoryless attachments for depth and MSAA targets

**Each of the five SHALL be a declared property of the pipeline that a check can read, not a
documented intention.** Every one of them is expressible in the render graph's own declarations — an
attachment's memoryless flag, a pass's subpass membership, a culling mode, the presence or absence of
a pass in the frame's pass set — and the null backend records every command and hashes the stream. A
difference that exists only as prose in this specification is a difference nothing can fail on.

**The pipeline SHALL therefore be assertable without a tiled GPU**, on the null backend, for its
structure — and the claim that structure makes about **hardware** SHALL NOT be reported as proven by
that assertion. The two are separate claims and they SHALL be recorded separately.

**A device claim no runner can judge SHALL report not evaluated with its reason**, per
`delivery-roadmap`'s mechanism for a question the available machines cannot ask. Reporting a pass for
a tile-memory property on a machine with no tiled GPU is the defect that mechanism exists to prevent,
and it is worse than reporting a failure.

#### Scenario: Tile memory is respected
- **WHEN** the mobile pipeline runs on a tiled GPU
- **THEN** the HDR colour attachment SHALL be declared memoryless and resolved to the swap chain
  within the same render pass

#### Scenario: The structure is checked with no device
- **WHEN** the mobile pipeline is configured on the null backend
- **THEN** the recorded command stream SHALL show the depth prepass absent, tonemapping inside the
  same render pass as shading, the screen-space passes absent, and the depth and MSAA attachments
  declared memoryless — and removing any one of those declarations SHALL make the check fail

#### Scenario: A structural check is not a hardware claim
- **WHEN** the mobile pipeline's structure passes on the null backend and no tiled GPU has run it
- **THEN** the tile-memory scenario SHALL be reported **not evaluated** with its reason, and the
  capability SHALL NOT be recorded as having satisfied it

#### Scenario: Per-object light lists are bounded
- **WHEN** the mobile pipeline shades an object under more lights than its per-object bound
- **THEN** the list SHALL be bounded by the declared limit and the overflow SHALL be reported through
  the pipeline diagnostics, rather than the bound being exceeded silently
