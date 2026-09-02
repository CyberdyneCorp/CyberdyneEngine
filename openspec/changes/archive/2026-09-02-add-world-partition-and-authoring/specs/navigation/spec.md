## MODIFIED Requirements

### Requirement: Navigation streaming
Navigation data SHALL be streamable with the world: tiles and regions loaded and unloaded as
content streams, with the hierarchy updated and in-flight queries remaining valid or failing
cleanly.

Navigation SHALL consume **cell lifecycle events** from `world-partition-and-streaming` to drive
tile residency, and navigation payloads SHALL be cooked as a cell channel so a server profile can
stream navigation without rendering data.

Navigation SHALL retain its **own tile layout**. Tile boundaries SHALL NOT be required to match
world cell boundaries, since the optimal partition for pathfinding differs from the optimal
partition for streaming.

Paths crossing into unloaded regions SHALL be resolvable at the abstract level, with local
refinement deferred until the region is resident.

#### Scenario: Path into unloaded content
- **WHEN** an agent paths toward a destination in a region that is not yet loaded
- **THEN** the abstract path SHALL be produced and local refinement deferred until that region
  streams in

#### Scenario: Region unloads under an agent
- **WHEN** a region unloads while an agent is pathing through it
- **THEN** the agent's path SHALL be invalidated cleanly and a repath triggered, rather than
  dereferencing released data

#### Scenario: Tiling is independent
- **WHEN** world cell size is changed
- **THEN** navigation tiling SHALL be unaffected, since it owns its own layout
