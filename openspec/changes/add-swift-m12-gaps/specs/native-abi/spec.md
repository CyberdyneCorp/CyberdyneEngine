# native-abi Spec Delta

## ADDED Requirements

### Requirement: Scheduled module systems
The interface SHALL let a module register a system: a name, the stage it runs in, and its access as
a list of (component, read | write | exclude) terms. The engine SHALL schedule it in that stage
beside native systems and order it against them by the same conflict rules, SHALL run it in the
update phase its stage belongs to, and SHALL refuse every structural change the body attempts while
it runs. After a hot reload the scheduled system SHALL run the new generation's code; a reload that
changes a scheduled system's stage or access SHALL be refused and the previous generation kept.

#### Scenario: Module system ordered against a native one
- **WHEN** a module system declares write access to a component and a native system in the same
  stage declares read access to it
- **THEN** the scheduler SHALL order the two, and two systems that only read SHALL not be ordered by
  that component

#### Scenario: Structural change refused inside a module system
- **WHEN** a module system's body calls an entity-creating entry while it runs
- **THEN** the call SHALL fail with `CY_RESULT_UNAVAILABLE` and no entity SHALL be created

#### Scenario: Reload moves a system to another stage
- **WHEN** a reload's new module registers an existing scheduled system in a different stage
- **THEN** the reload SHALL be refused, and the system SHALL keep running the previous generation's
  code in its original place

### Requirement: Behaviour tree callbacks
The behaviour vtable SHALL carry the tree callbacks `enter_tree`, `ready`, `enable`, `disable` and
`exit_tree`. For an instance attached to a scene node the engine SHALL call them at the scene tree's
pump in the tree's order — `enter_tree` parent first and once per attachment, `ready` child first,
`exit_tree` child first, `enable`/`disable` on a change of effective enablement — through the vtable
of the generation that created the instance. A module compiled before they were appended SHALL get
none of them.

#### Scenario: A subtree attached in one frame
- **WHEN** a parent node and its child, each with a module behaviour, are created under the root
  before one pump
- **THEN** the pump SHALL call `enter_tree` on the parent then the child, then `ready` on the child
  then the parent, each exactly once

### Requirement: Node paths, bodies and characters
The interface SHALL resolve a node path relative to a node or from the root (`node_find`), SHALL let
a module apply forces, impulses and torques to and set the velocity of the body an entity owns, and
SHALL give an entity a capsule character controller that a fixed step moves. Body writes and
character lifetimes SHALL be refused in frame update; a character move SHALL be refused outside a
fixed step and SHALL use the fixed delta; a write to a body that cannot move SHALL be refused rather
than ignored; every body and character call SHALL answer `CY_RESULT_UNAVAILABLE` while the physics
step runs.

#### Scenario: A path that does not resolve
- **WHEN** `node_find` is asked for a path naming no node
- **THEN** it SHALL return `CY_RESULT_NOT_FOUND` and leave its output untouched

#### Scenario: A push on a static body
- **WHEN** a module applies an impulse to an entity whose body is static
- **THEN** the call SHALL return `CY_RESULT_INVALID_ARGUMENT`

#### Scenario: A character move in frame update
- **WHEN** a module calls `character_move` during frame update
- **THEN** the call SHALL return `CY_RESULT_PERMISSION_DENIED` and the character SHALL not move
