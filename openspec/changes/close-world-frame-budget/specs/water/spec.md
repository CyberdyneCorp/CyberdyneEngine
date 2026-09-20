# Spec Delta

## ADDED Requirements

### Requirement: Rendering foam is device-owned visual state
Foam that affects only the rendered water surface SHALL be evolved on the graphics device from the
authoritative water and weather inputs. Saves, replays, lockstep simulation, procedural generation,
and gameplay queries SHALL NOT depend on this visual foam state.

#### Scenario: A rendered water frame advances
- **WHEN** the world renders after wind or breaking-wave inputs change
- **THEN** a device dispatch SHALL advect and decay visual foam and the water draw SHALL consume the resulting state

#### Scenario: The world runs headless
- **WHEN** the same authoritative take runs without a graphics device
- **THEN** it SHALL omit visual foam evolution and produce the same authoritative world digest
