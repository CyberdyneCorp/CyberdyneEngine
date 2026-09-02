## MODIFIED Requirements

### Requirement: Decals
Decals SHALL be projected oriented boxes writing into albedo, normal, roughness, metallic, and
emission before lighting, blended by per-channel weights.

Decals SHALL be **GPU scene residents**, not per-decal draw calls: a decal is an instance with a
transform, bounds, a material reference, and sort order, culled and gathered on the GPU like any
other instance, and applied through tile or cluster assignment.

The system SHALL be designed for very large decal counts — persistent damage, bullet impacts,
splatter — with a **decal budget** governed by the renderer budget arbiter and eviction of the
least important decals by age, screen coverage, and importance when the budget is reached.

Decals SHALL support: angle-based fade against the receiving surface normal, distance fade, a sort
order, a layer mask, and normal blending that preserves the receiver's detail.

Decals SHALL participate in cluster assignment as a distinct element type.

#### Scenario: Decal on a steep surface
- **WHEN** the receiving normal deviates beyond the decal's fade angle
- **THEN** the decal SHALL fade out rather than stretch

#### Scenario: Decal ordering
- **WHEN** decals overlap
- **THEN** they SHALL be applied in sort order, so a later decal can cover an earlier one

#### Scenario: Many decals, no draw call per decal
- **WHEN** tens of thousands of impact decals are present
- **THEN** they SHALL be culled and applied as GPU scene instances, without a CPU draw call each

#### Scenario: Budget evicts the least important
- **WHEN** the decal budget is reached and a new decal is spawned
- **THEN** the least important existing decal SHALL be evicted deterministically, and the eviction
  SHALL be reportable
