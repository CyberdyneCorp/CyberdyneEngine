## MODIFIED Requirements

### Requirement: Terrain modifier stack
Terrain authoring SHALL be **non-destructive**: a terrain SHALL be defined as a generator plus an
ordered stack of modifiers — noise, erosion, spline roads, river carving, area flattening, craters,
and artist sculpting — each of which can be reordered, disabled, or edited after later ones exist.

Generators and modifiers SHALL be **procedural programs** (see
`procedural-content-generation`): they are authored as graphs, compiled to programs, executed by
region, cached by derivation key, and invalidated by the dependency and radius declarations that
capability defines. Terrain SHALL NOT maintain a separate procedural execution model.

Modifiers SHALL write **stamps and layers** into the stack rather than destructively rewriting the
source heightfield.

Cooking SHALL flatten the stack; the runtime SHALL NOT carry it.

Stack evaluation SHALL be deterministic, so a terrain cooks identically from the same inputs, and
SHALL be incremental: editing one modifier SHALL invalidate only the regions its bounds and declared
radius reach.

#### Scenario: Reordering does not require redoing work
- **WHEN** an author inserts an erosion pass beneath an existing sculpt
- **THEN** the sculpt SHALL be preserved and reapplied above it

#### Scenario: Cooking flattens
- **WHEN** terrain is cooked
- **THEN** the runtime SHALL receive tile data, and the modifier stack SHALL be absent

#### Scenario: One road edit is scoped
- **WHEN** a road spline is moved
- **THEN** only the terrain regions its stamp and declared radius reach SHALL be re-evaluated
