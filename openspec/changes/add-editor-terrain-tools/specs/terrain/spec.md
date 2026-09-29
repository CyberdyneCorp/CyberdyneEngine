# terrain Spec Delta

## ADDED Requirements

### Requirement: Terrain brush modifiers
The modifier stack SHALL carry editor brush strokes (raise, lower, smooth, flatten, material paint
and hole) as modifiers of a stroke of dabs with a radius, strength, falloff and per-dab pressure. A
brush SHALL change heights, material weights and holes only within the union of its dabs' discs. A
smooth brush SHALL declare a halo that covers its radius and every pass, so that adjacent tiles
agree on their shared boundary. A painted texel SHALL keep at most the bounded number of layers
with weights summing to the full range. The engine SHALL evaluate an author's region from the
stack, and SHALL mesh and collide it with the same meshing and collision builders the runtime uses,
so that a hole the author sees is absent from both rendering and collision.

#### Scenario: A brush changes only its footprint
- **WHEN** a raise, lower, smooth, flatten, paint or hole stroke is added to a stack
- **THEN** every sample and texel farther than the radius from every dab SHALL be unchanged

#### Scenario: Removing a stroke restores the region
- **WHEN** a region is evaluated from a stack, from that stack with a stroke added, and from the stack without it again
- **THEN** the last evaluation's heights, weights and holes SHALL be byte-identical to the first

#### Scenario: A hole reaches rendering and collision
- **WHEN** a hole stroke is evaluated
- **THEN** meshing SHALL omit exactly the cut quads and collision SHALL mark hole samples

### Requirement: Edited terrain marks navigation stale
The engine's terrain editing service SHALL mark as navigation-dirty the reach of every modifier that
is added, removed or changed between two evaluations of the same terrain. Those regions SHALL stay
marked until navigation is rebaked, and SHALL be reported to the editor with every evaluation.

#### Scenario: A stroke marks its reach
- **WHEN** a stroke is added to a terrain the service has already evaluated
- **THEN** the service SHALL report its dabs' extent expanded by its radius, clipped to the region, as stale

#### Scenario: Stale regions persist across an undo
- **WHEN** that stroke is then removed
- **THEN** its region SHALL still be reported stale, once
