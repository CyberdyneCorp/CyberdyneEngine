# asset-import-pipeline Spec Delta

## ADDED Requirements

### Requirement: Visible asynchronous import workflow
The Content Browser SHALL provide an Import… interaction and SHALL accept operating-system file
drops. Both gestures SHALL copy accepted external sources into a project-owned source location and
invoke the same registered import command used by agents.

Import SHALL execute without blocking the interface thread. Each queued import SHALL have a stable
request identity, observable progress, cooperative cancellation and a retained structured terminal
result. Cancelling SHALL leave every already-published cache entry and sidecar valid.

#### Scenario: Import button remains responsive
- **WHEN** an author selects a large FBX through Import…
- **THEN** the Content Browser SHALL show its progress and cancellation affordance
- **AND** unrelated editor interaction SHALL remain responsive

#### Scenario: File drop uses the command path
- **WHEN** an author drops a supported external model into the Content Browser
- **THEN** the editor SHALL copy it into the project and submit the registered import command
- **AND** the command and agent result SHALL use the same result schema

#### Scenario: Cancellation leaves valid state
- **WHEN** an author cancels an import between importer steps
- **THEN** no incomplete asset SHALL be published
- **AND** previously completed cache entries and sidecars SHALL remain usable

### Requirement: Imported sub-assets are browsable and instantiable
Every extracted mesh, material and texture SHALL be indexed as a logical asset carrying stable
sub-asset identity, kind, display name, source and dependency/binding metadata. The Content Browser
SHALL expose those logical assets without inventing paths that lose their source relationship.

Placing a model whose primary output is a prefab SHALL instantiate its complete node hierarchy,
local transforms, mesh references and material-slot bindings in one transaction. Reimport SHALL
preserve references when stable sub-asset identities are unchanged.

#### Scenario: Extracted material is selectable
- **WHEN** an FBX import produces mesh, material and texture sub-assets
- **THEN** each SHALL appear as its own typed Content Browser entry with a stable identity

#### Scenario: Model placement preserves hierarchy and materials
- **WHEN** an author places an imported model with nested nodes and material slots
- **THEN** the created entities SHALL preserve the hierarchy, local transforms and bindings
- **AND** one undo SHALL remove the complete instance without removing imported assets

#### Scenario: Reimport preserves instance references
- **WHEN** a source is reimported without changing a referenced sub-asset identity
- **THEN** existing scene instances SHALL continue to resolve that mesh or material

#### Scenario: External OBJ companions are staged together
- **WHEN** an author imports an external OBJ which declares an MTL and texture files
- **THEN** the editor SHALL copy the resolvable declared companions while preserving relative paths
- **AND** missing companions SHALL remain importer diagnostics rather than partial editor assets

#### Scenario: Mesh sections retain material slots
- **WHEN** an imported mesh contains surfaces using more than one material index
- **THEN** the import result SHALL carry stable material identities indexed by slot
- **AND** save, reload and reimport SHALL preserve those slot references
