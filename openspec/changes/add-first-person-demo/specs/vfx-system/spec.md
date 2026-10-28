## ADDED Requirements

### Requirement: A spawn that ends
An effect instance SHALL be playable with a spawn duration: once its age reaches the duration and
it has simulated at least one sub-step, it SHALL stop spawning and let its particles live out their
lifetimes, and a fire-and-forget instance SHALL then release itself when its last particle dies.
An instance played without a duration SHALL spawn until it is stopped.

#### Scenario: A burst nobody holds releases itself
- **WHEN** a fire-and-forget burst is played with a spawn duration and never stopped
- **THEN** it SHALL stop spawning after the duration and its pool block SHALL be released once its last particle has died

#### Scenario: A duration shorter than a sub-step
- **WHEN** a spawn duration is shorter than the instance's simulation step
- **THEN** the instance SHALL still spawn on its first sub-step before it stops
