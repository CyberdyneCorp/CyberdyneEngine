# editor-architecture Spec Delta

## ADDED Requirements

### Requirement: Catalogue-driven material properties and acknowledged preview
The Material Graph SHALL render property controls exclusively from versioned backend catalogue
descriptors. Descriptors SHALL include stable property identity, value type, typed default,
constraints, authoring metadata and any required asset kind. Texture properties SHALL select from
project texture assets and SHALL serialize stable asset identities.

After successful compilation the editor SHALL create or reuse an isolated preview world, request an
artefact reload and wait for an acknowledgement containing requested and applied artefact identities
before reporting the viewport current. Failed or stale reloads SHALL retain the previous preview.

#### Scenario: Texture property needs no node-specific editor code
- **WHEN** the catalogue declares a texture asset property
- **THEN** the editor SHALL render a texture selector filtered to texture assets
- **AND** shall not branch on the material node's name

#### Scenario: Compile becomes visible only after acknowledgement
- **WHEN** a material compile succeeds and the preview accepts its artefact
- **THEN** the editor SHALL report the applied artefact identity and show it in the viewport

#### Scenario: Rejected reload keeps the old material
- **WHEN** the runtime rejects a compiled artefact reload
- **THEN** the previous preview artefact SHALL remain applied
- **AND** the editor SHALL show the structured rejection

#### Scenario: Compilation targets the selected viewport mesh
- **WHEN** an author compiles a material while one or more mesh entities are selected
- **THEN** the reload SHALL name every selected entity and material-slot identity
- **AND** the viewport SHALL become current only after the runtime acknowledges those exact bindings

#### Scenario: Runtime parameter values are typed
- **WHEN** the editor updates a live parameter
- **THEN** the request SHALL name the preview generation, applied artefact generation, stable
  parameter identity and explicit value type
- **AND** stale generations, unknown parameters and type changes SHALL be structured refusals

### Requirement: Terrain and VFX specialised editors are operational
The terrain specialised editor SHALL provide heightfield import, non-destructive sculpting and
material-layer painting through the shared painting surface and ordinary transactions. The VFX
specialised editor SHALL use the shared graph canvas and backend catalogue, validation, compilation
and preview services.

#### Scenario: Terrain gesture is undoable
- **WHEN** an author completes one sculpt or paint gesture
- **THEN** it SHALL be recorded as one transaction over stable terrain modifier or layer identities

#### Scenario: VFX editor contains no lowering implementation
- **WHEN** an author compiles a VFX graph
- **THEN** the editor SHALL submit it through editor backend services
- **AND** SHALL NOT link VFX lowering, shader compiler, renderer or Metal implementation code
