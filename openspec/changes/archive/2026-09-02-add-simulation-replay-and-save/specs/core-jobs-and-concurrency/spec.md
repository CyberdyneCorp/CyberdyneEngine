## MODIFIED Requirements

### Requirement: Deterministic scheduling mode
The scheduler SHALL support a **deterministic mode** in which system execution order is a fixed
topological order of the dependency graph, independent of worker timing, and parallel loops use a
fixed partitioning.

In deterministic mode: deadline hints SHALL be ignored, parallel reductions SHALL use their fixed
combination order, and command and event commit SHALL follow their declared deterministic order.

Ordering keys used for commit SHALL be built from stable logical identity — system, partition, local
sequence — and **SHALL NOT include worker or thread identity**, since work stealing makes a worker's
identity a function of timing.

Determinism SHALL NOT require single-threaded execution. Parallel execution SHALL remain
deterministic through fixed partitioning and ordered commit rather than through serialisation.

The engine SHALL also provide a **single-threaded deterministic** mode for debugging, in which the
same results are produced without parallelism, so a discrepancy between the two modes localises a
scheduling-dependent defect.

The scheduler SHALL additionally provide a **chaos mode** that deliberately randomises permitted
execution order, worker counts, and chunk assignment, so that undeclared ordering dependencies
surface in testing rather than in production (see `simulation-and-determinism`).

#### Scenario: Reproducible run for testing
- **WHEN** deterministic mode and a fixed simulation step are enabled
- **THEN** two runs with identical inputs SHALL produce identical world state

#### Scenario: Parallel and single-threaded agree
- **WHEN** the same inputs are run in deterministic parallel mode and single-threaded deterministic
  mode
- **THEN** the results SHALL be identical, and any divergence SHALL indicate a defect in a
  system's ordering assumptions

#### Scenario: Chaos exposes a hidden dependency
- **WHEN** chaos mode randomises permitted ordering
- **THEN** a system whose result depends on execution order SHALL produce differing results and be
  identified
