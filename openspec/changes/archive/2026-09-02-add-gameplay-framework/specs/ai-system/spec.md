## MODIFIED Requirements

### Requirement: Gameplay API
AI SHALL be exposed to gameplay through components and a declarative authoring surface in Swift
and C++, with the scripting layer describing behaviour and the engine performing scheduling,
perception, and navigation natively.

Scripts SHALL be able to define: conditions, actions, utility considerations and scoring, GOAP
actions with preconditions and effects, and custom sensors and environment-query tests.

**AI acts through gameplay commands.** An agent that moves, attacks, builds, or interacts SHALL emit
the same commands a player emits (see `gameplay-framework`), and SHALL NOT call gameplay logic
directly. Consequently AI is exercised by the same validation as players, is recorded by replay, and
can be driven by a human or replaced by one without gameplay changes.

AI SHALL be able to **validate a command without issuing it**, so planning can ask whether an action
would be permitted and read the structured reason when it would not — the same validation the
interface and the authority use.

Script-defined nodes SHALL declare their data access so the scheduler can parallelise agent
thinking safely, exactly as for systems.

Script code SHALL NOT be invoked per agent per tick for agents whose graph is executing built-in
nodes only.

#### Scenario: Declarative action in Swift
- **WHEN** a developer defines a utility action with a scoring function in Swift
- **THEN** it SHALL be registered as a graph node, invoked only when that node is evaluated, and
  scheduled according to its declared access

#### Scenario: Native fast path
- **WHEN** an agent's think executes only built-in nodes
- **THEN** no script call SHALL occur for that agent that tick

#### Scenario: An agent is indistinguishable from a player
- **WHEN** an agent orders a unit to move
- **THEN** it SHALL emit the same command a player would, and the simulation SHALL not distinguish
  them

#### Scenario: Planning asks before acting
- **WHEN** an agent considers building a structure
- **THEN** it SHALL validate the command without issuing it and SHALL read the structured reason if
  it is not permitted
