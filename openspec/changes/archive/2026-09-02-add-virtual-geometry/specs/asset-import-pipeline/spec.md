## ADDED Requirements

### Requirement: Virtual geometry cooking
The import pipeline SHALL support cooking meshes into virtual geometry (see `virtual-geometry`),
as a per-asset option with project-level defaults.

Cooking SHALL: build clusters per the cooker's cluster policy, group and simplify iteratively to
build the hierarchy with crack-free boundaries, compute geometric error per node, pack clusters
into content-addressed pages, produce the always-resident root region, and generate the fallback
representation.

Cooking SHALL be **deterministic** — the same source and settings produce byte-identical output —
so the content-addressed cache and patching work.

Because virtual geometry cooking is expensive, it SHALL be cache-friendly at a fine granularity:
unchanged assets SHALL never be rebuilt, and the shared cook cache SHALL be usable so that CI can
populate it for the team.

Collision and navigation representations SHALL be derived **independently** from the source mesh
with their own complexity budgets, never from the virtual geometry representation.

The cooker SHALL report per asset: source triangles, cluster count, hierarchy depth, page count,
cooked size, always-resident size, bytes per triangle, quantisation error, and any content
suitability warnings.

#### Scenario: Deterministic cook
- **WHEN** an asset is cooked twice with identical settings
- **THEN** the output SHALL be byte-identical, so content addressing and patching are valid

#### Scenario: Expensive cook is cached
- **WHEN** a large asset is unchanged between builds
- **THEN** its cooked virtual geometry SHALL be taken from the cache rather than rebuilt

#### Scenario: Collision is independent
- **WHEN** a 50-million-triangle asset is cooked
- **THEN** its collision representation SHALL be generated from the source mesh to its own budget,
  and SHALL NOT derive from the render clusters

#### Scenario: Suitability is reported
- **WHEN** an asset's topology is poorly suited to virtual geometry
- **THEN** the cook SHALL warn with the reason rather than silently producing a poor hierarchy
