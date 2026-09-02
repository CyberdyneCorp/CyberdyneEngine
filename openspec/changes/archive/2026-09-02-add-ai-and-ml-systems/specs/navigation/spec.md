## ADDED Requirements

### Requirement: Flow fields
The engine SHALL support **flow fields**: a grid over a navigable region where each cell stores a
direction toward a shared destination, so that many agents heading to the same place are guided by
one computed field rather than by one path each.

Flow fields SHALL support: generation from one or more destinations, integration cost derived from
navigation area costs and dynamic obstacles, incremental regeneration when the underlying
navigation or obstacles change, a bounded region of interest, and reference counting so a field is
released when no agent uses it.

Agents SHALL be able to switch between individual pathfinding and flow-field following, selected by
their AI LOD tier or explicitly.

Flow-field generation SHALL be deterministic.

#### Scenario: Many agents, one field
- **WHEN** 20,000 agents are ordered to the same destination
- **THEN** one flow field SHALL guide them, rather than 20,000 individual path queries

#### Scenario: Field reacts to a change
- **WHEN** a building is destroyed, opening a route
- **THEN** the affected region of the field SHALL be regenerated incrementally rather than the
  whole field recomputed

#### Scenario: Field is released
- **WHEN** the last agent following a field stops using it
- **THEN** the field SHALL be released

#### Scenario: Local avoidance still applies
- **WHEN** agents follow a flow field
- **THEN** local avoidance SHALL still resolve agent-to-agent collision, since the field encodes
  direction, not separation

### Requirement: Hierarchical pathfinding
For large worlds, the engine SHALL support **hierarchical pathfinding**: navigation data organised
into a hierarchy of regions, with an abstract graph over region connections above the polygon
graph.

A long-distance query SHALL first plan over the abstract graph, then refine only the portions
needed for immediate movement, refining further as the agent advances.

The hierarchy SHALL be built incrementally as navigation data is generated or streamed, and
SHALL be invalidated regionally when the underlying navigation changes.

#### Scenario: Cross-map path
- **WHEN** an agent paths across a large world
- **THEN** the search SHALL run over the abstract region graph and refine locally, rather than
  searching the full polygon graph

#### Scenario: Progressive refinement
- **WHEN** an agent advances along an abstract path
- **THEN** subsequent segments SHALL be refined as needed, spreading cost over time

#### Scenario: Regional invalidation
- **WHEN** navigation changes in one region
- **THEN** only that region's abstract connections SHALL be recomputed

### Requirement: Navigation volumes
The engine SHALL support **navigation volumes** for agents that move in three dimensions —
flying, swimming, and space — as a sparse voxel or octree representation of navigable space,
distinct from surface navigation meshes.

Volumes SHALL support pathfinding with the same query interface as meshes: costs, area masks,
budgets, partial paths, and async queries with deterministic completion.

An agent SHALL declare which navigation representation it uses; a world MAY contain both.

#### Scenario: Flying agent
- **WHEN** a flying agent paths through a canyon
- **THEN** it SHALL query the navigation volume and receive a three-dimensional path

#### Scenario: Mixed representations
- **WHEN** a world contains both walking and flying agents
- **THEN** each SHALL use its declared representation, and the query interface SHALL be identical

### Requirement: Navigation streaming
Navigation data SHALL be streamable with the world: tiles and regions loaded and unloaded as
content streams, with the hierarchy updated and in-flight queries remaining valid or failing
cleanly.

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

### Requirement: Crowd simulation
Crowd movement SHALL be a scheduled, data-oriented system operating over agent components in bulk,
not per-agent object updates.

The crowd system SHALL integrate: path or flow-field following, local avoidance, separation and
formation behaviours, speed and acceleration limits, and priority so that important agents are
less likely to yield.

Crowd cost SHALL be tiered consistently with AI LOD: agents at reduced tiers SHALL use cheaper
avoidance and coarser movement.

#### Scenario: Large crowd
- **WHEN** 20,000 agents move simultaneously
- **THEN** movement SHALL be processed as parallel systems over packed component arrays with
  spatially partitioned neighbour queries

#### Scenario: Priority in a bottleneck
- **WHEN** agents of differing priority meet at a chokepoint
- **THEN** lower-priority agents SHALL yield, avoiding deadlock

#### Scenario: Tiered avoidance
- **WHEN** agents are at a reduced AI LOD tier
- **THEN** they SHALL use cheaper avoidance, with the fidelity difference documented

## MODIFIED Requirements

### Requirement: Pathfinding
Path queries SHALL run **A\*** over the polygon adjacency graph with a Euclidean heuristic,
producing a **corridor** of polygons, then a point path.

Queries SHALL support: area cost multipliers per agent (so an agent can prefer roads or avoid
water), an area mask excluding types entirely, a maximum search node budget, and partial paths
when the target is unreachable.

Queries SHALL be issuable synchronously and asynchronously, and SHALL be safe from parallel
systems.

Asynchronous queries SHALL complete **deterministically**: a query issued on a given tick SHALL
deliver its result on a defined later tick and be applied in a deterministic order, so that AI
decisions depending on path results remain reproducible (see `ai-system`).

For destinations shared by many agents, flow fields SHALL be preferred over per-agent queries, and
for long distances in large worlds, hierarchical pathfinding SHALL be used.

#### Scenario: Unreachable target
- **WHEN** no path exists
- **THEN** the query SHALL return the reachable point closest to the target, flagged as partial,
  rather than failing silently

#### Scenario: Area preferences
- **WHEN** an agent has a high cost for "water" areas
- **THEN** A* SHALL route around water unless the detour exceeds the cost difference

#### Scenario: Search budget exceeded
- **WHEN** a query exceeds its node budget
- **THEN** it SHALL return the best partial path found and report that the budget was hit

#### Scenario: Deterministic async delivery
- **WHEN** the same scenario is replayed
- **THEN** async path results SHALL be applied on the same ticks in the same order, regardless of
  how long each query took to compute
