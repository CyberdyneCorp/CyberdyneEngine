## ADDED Requirements

### Requirement: Import from inside the editor
Import SHALL be invocable from the editor as a **registered command**, with the same typed
parameters, description and declared effect class every other command carries — so that a person
importing a model does not leave the editor, and so that an agent can import at all: the agent
interface is a projection of the command registry, and a capability that is not a command is not a
tool.

The command SHALL report, in a machine-readable result, which importer ran, the identity the source
holds, what the cache did, and every sub-asset the import produced with the identity bound to it. A
caller SHALL NOT have to parse a human-readable report to learn what an import produced.

What the import produced SHALL be placeable into the open world as an entity through the ordinary
transaction path, so that undo removes the entity. Undo SHALL NOT remove the cooked assets: they
belong to the source file rather than to the world, exactly as a file copied into the project does.

A source whose extension no registered importer claims SHALL be refused, and the refusal SHALL name
the extensions this build does import — read from the importers themselves, so that an importer a
project registers appears in that list without an editor change.

#### Scenario: An agent imports a model
- **WHEN** an agent lists the editor's tools
- **THEN** import SHALL be among them, with its parameters, its description and its effect class,
  and invoking it SHALL cook the asset and place it in the open world

#### Scenario: Undo removes the entity and not the asset
- **WHEN** an imported mesh has been placed in a world and the import is undone
- **THEN** the world SHALL be exactly as it was, and the cooked assets and their sidecars SHALL
  remain

#### Scenario: A format nothing claims is refused with the list
- **WHEN** a file whose extension no importer claims is imported
- **THEN** it SHALL be refused, and the refusal SHALL name the extensions this build imports

### Requirement: A format's absent steps are declared by the importer
An importer SHALL declare which of the model import sequence's steps it reaches, and the import
report SHALL name the steps it did not reach. That declaration is a property of the **importer**
rather than of any one import, so a cache hit reports the same absent steps a miss does.

Naming an absent step SHALL NOT be a warning, SHALL NOT be counted among the warnings, and SHALL NOT
be worded as a deficiency in the file.

#### Scenario: A cache hit reports the same absent steps as a miss
- **WHEN** an OBJ is imported twice and the second import is a cache hit
- **THEN** both reports SHALL name steps 7, 8 and 10 as not reached, and neither SHALL report a
  warning for them

#### Scenario: A texture importer has no steps to have skipped
- **WHEN** a texture is imported
- **THEN** the report SHALL say nothing about the model import sequence, because it does not apply
