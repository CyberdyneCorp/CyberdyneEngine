## MODIFIED Requirements

### Requirement: Behaviours bridge nodes and systems
A **behaviour** SHALL be a script-side or native object attached to a node, declaring lifecycle
callbacks and the component data it reads and writes.

Behaviours SHALL be **compiled** where possible: a behaviour whose callbacks operate on declared
component data SHALL be lowered into a **generated system** iterating chunks, so that many instances
of one behaviour cost one system rather than one call per instance.

Behaviours that cannot be batched — those invoking arbitrary script per entity per tick, holding
unbounded per-instance state, or accessing data outside their declaration — SHALL fall back to
per-instance dispatch, and the build SHALL **report which behaviours batched, which did not, and
why**.

Per-instance dispatch SHALL remain a system iterating entities with a behaviour reference, so script
execution is scheduled and ordered like any other system.

Behaviours are the ergonomic path and systems the explicit one; the compiler is what keeps the
ergonomic path from being the slow one.

#### Scenario: Behaviour dispatch is a system
- **WHEN** the `Simulation` stage runs
- **THEN** behaviour execution SHALL occur through systems — generated where batchable, dispatching
  where not — ordered and scheduled like other systems

#### Scenario: Guidance on scale
- **WHEN** a project needs per-entity logic on 100 000 entities
- **THEN** a batchable behaviour SHALL compile to one system, and where a behaviour cannot batch the
  build SHALL report it so the developer can convert it deliberately

#### Scenario: The cost is known at build time
- **WHEN** a behaviour prevents batching
- **THEN** the build report SHALL name it and the reason, rather than the cost appearing only at
  scale
