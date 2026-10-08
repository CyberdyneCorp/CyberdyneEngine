# animation-and-skinning Spec Delta

## ADDED Requirements

### Requirement: The pose vocabulary validates as an editor draws it
The pose node vocabulary SHALL declare the pins a graph is wired with: a state SHALL have a `state`
output that a transition's `from` and `to` inputs take, so a state machine drawn output to input
validates against the registry. A clip node that names no time parameter SHALL have a clock of its own,
not one shared with every other clip that names none.

#### Scenario: A built state machine validates
- **WHEN** the locomotion builder writes its four-state machine
- **THEN** validating the graph against the pose vocabulary SHALL report no error

#### Scenario: Two unnamed clocks are two clocks
- **WHEN** two clip nodes name no time parameter
- **THEN** their sample instructions SHALL read two different parameters
