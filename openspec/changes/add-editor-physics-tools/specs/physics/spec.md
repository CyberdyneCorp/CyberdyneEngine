# physics Spec Delta

## ADDED Requirements

### Requirement: Joints authored in a world
A joint SHALL be authorable in a world as a component on the entity carrying its first body, naming
the second body's entity by reference or none for the world, with its kind, anchor and axis in the
first body's frame, and the limits, motor, ratio, break thresholds and collision flag its kind reads.
Every constraint kind the engine provides SHALL be authorable. At play the engine SHALL create the
constraint between the two bodies with both anchors coinciding where the bodies were authored. A
joint whose kind is unknown or whose bodies do not exist SHALL be refused and counted without
stopping the rest of the world simulating.

Every change to an authored joint SHALL be one undoable transaction reachable by an agent, and a
change the engine would refuse at play SHALL be refused when it is made.

#### Scenario: An authored point joint holds its body
- **WHEN** a body is joined to a static body by an authored point joint and play runs
- **THEN** the body SHALL stay on the arm the anchors define, and the same world without the joint SHALL let it fall

#### Scenario: A joint to a body that does not exist
- **WHEN** an authored joint names an entity with no body
- **THEN** play SHALL refuse that joint, count it, and simulate everything else

#### Scenario: A field change undoes alone
- **WHEN** one field of an authored joint is changed and then undone
- **THEN** exactly that field SHALL return to its previous value
