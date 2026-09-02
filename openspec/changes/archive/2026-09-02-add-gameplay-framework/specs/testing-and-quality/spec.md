## MODIFIED Requirements

### Requirement: Performance benchmarks
The engine SHALL maintain benchmarks covering: ECS iteration and structural change throughput,
job system scheduling overhead, culling throughput, draw submission and render graph compilation,
physics step time at defined body counts, asset load and cook time, Swift call overhead across
the ABI, and **gameplay framework overhead** against the targets it declares.

The suite SHALL include **acceptance scenarios** exercising the architecture rather than a
favourable case:

| Scenario | Exercises |
|---|---|
| **Strategy stress** | 8 participants, 4 teams, 100 000 units with 20 000 moving, 5 000 agent groups, 1 000 structures, hundreds of commands per frame, world streaming, network authority, and replay recording |
| **Control handover** | 4-player networked co-operative play: a player enters and exits a vehicle, an AI takes it, a second player operates the turret, prediction and control transfer, a spectator observes, and a replay reconstructs it |
| **Headless server** | A dedicated server with no renderer, GPU, audio, or interface, running 100 000 entities with AI, commands, and physics at a fixed simulation rate |

The strategy scenario SHALL be treated as the primary architectural test, since a single-character
scenario does not distinguish a data-oriented gameplay framework from an object-oriented one.

Benchmarks SHALL run nightly on stable hardware, record results over time, and fail when a metric
regresses beyond a per-benchmark threshold.

Each benchmark SHALL declare what it measures and what a regression would mean, so a failure is
actionable rather than mysterious.

#### Scenario: Regression detected
- **WHEN** ECS iteration throughput drops more than the threshold
- **THEN** the nightly run SHALL fail with the commit range and a history chart

#### Scenario: Intentional trade-off
- **WHEN** a change trades throughput for a correctness fix
- **THEN** the threshold SHALL be updated in the same change with a recorded justification

#### Scenario: The architecture is tested, not a demo
- **WHEN** gameplay framework performance is assessed
- **THEN** the strategy stress scenario SHALL be the reference, and framework overhead SHALL be
  reported as a fraction of simulation time

#### Scenario: Headless is verified continuously
- **WHEN** the headless scenario runs
- **THEN** it SHALL execute with no rendering, audio, or interface code linked, and a dependency on
  any of them SHALL fail the build
