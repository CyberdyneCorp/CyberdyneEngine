## ADDED Requirements

### Requirement: Authoring against the engine is one command
The workflow SHALL provide a recipe that creates a project from the editor's templates, a recipe
that runs the engine host on a project until it is interrupted, and a recipe that runs the engine
host and an editor attached to it on one project.

The run recipes SHALL resolve the project, the world, and the transport endpoints in one shared
place, so that the engine and the editor always name the same world and the same endpoints. A
relative project path SHALL resolve against the directory the recipe was invoked from. Closing the
attached editor SHALL stop the engine host the recipe started.

#### Scenario: From nothing to an attached editor
- **WHEN** a contributor runs the project-creation recipe on an empty directory and then the live
  recipe on that directory
- **THEN** the engine host SHALL load the project's world, and an editor window SHALL open on the
  same world attached to it

#### Scenario: Two projects do not collide
- **WHEN** two projects are run at the same time
- **THEN** their engine hosts SHALL listen on different endpoints

#### Scenario: A session does not time out
- **WHEN** the engine host is started by the run recipe
- **THEN** it SHALL run until it is interrupted or the attached editor closes, not for a fixed time
