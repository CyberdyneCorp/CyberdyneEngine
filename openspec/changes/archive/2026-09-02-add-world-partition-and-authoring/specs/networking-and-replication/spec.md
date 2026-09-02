## MODIFIED Requirements

### Requirement: Replication cells
Replicated state SHALL be organised into **replication cells**: a spatial partition used to
accelerate relevance queries, to scope baselines, and to coordinate with content streaming.

Replication cells SHALL derive from the **world partition** (see
`world-partition-and-streaming`) rather than from a partition owned by networking. Networking SHALL
NOT maintain a second spatial subdivision of the same world.

Networking MAY aggregate world cells into coarser replication cells where relevance granularity
differs from streaming granularity, but the aggregation SHALL be derived from world cell identity,
so client and server refer to the same content by the same identifiers.

A peer's interest SHALL be expressed as a set of cells derived from its viewpoint, and entering or
leaving a cell SHALL be an explicit event that other systems can observe.

Cell membership SHALL drive **asset preloading** and **world content streaming**, so that a client
entering a cell can have its content resident before entities arrive: the server's relevance
decision SHALL be able to issue a streaming hint to the client ahead of replicating the entities.

#### Scenario: Cell entry drives preloading
- **WHEN** a client's viewpoint approaches a new cell
- **THEN** the cell-entry event SHALL be observable so world and asset streaming can preload its
  content before replicated entities are spawned

#### Scenario: Relevance query is bounded
- **WHEN** relevance is computed for a peer in a large world
- **THEN** only entities in the peer's interest cells SHALL be considered, rather than all
  entities

#### Scenario: Partition source is replaceable
- **WHEN** the world partition's configuration changes
- **THEN** replication cells SHALL follow it without changes to network identity, interest
  scoring, or the scheduler

#### Scenario: One spatial model
- **WHEN** the server evaluates relevance and the client streams content
- **THEN** both SHALL refer to the same world cell identities, with no separate networking
  partition to keep in agreement
