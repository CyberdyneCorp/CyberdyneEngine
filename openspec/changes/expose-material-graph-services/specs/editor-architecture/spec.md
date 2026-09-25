# editor-architecture Spec Delta

## ADDED Requirements

### Requirement: Material editor is a backend-service client
The material editor SHALL obtain its catalogue, validation, compilation, preview worlds, parameter
updates, and reload results exclusively through `editor-backend-services` over the stable C ABI or
live bridge.

The editor SHALL NOT compile or link renderer, material compiler, shader compiler, graphics backend,
Metal, or VFX implementation code. It SHALL retain documents and pending edits if a service session
is lost and SHALL reconcile results only by stable request and graph identities. The application
chrome SHALL visibly distinguish an unavailable, loading, ready, or failed engine catalogue so a
connected runtime is not mistaken for a material-authoring backend that is ready.

#### Scenario: Catalogue changes without editor recompilation
- **WHEN** an engine build adds a material node while retaining compatible service schemas
- **THEN** the same editor build SHALL display it from the returned catalogue

#### Scenario: Runtime loss preserves authored work
- **WHEN** the runtime exits during material compilation
- **THEN** the editor SHALL keep the graph and pending edits and mark the request failed because the session was lost

#### Scenario: Engine catalogue readiness is visible
- **WHEN** the runtime returns a compatible material catalogue
- **THEN** the editor SHALL report that the engine catalogue is ready without requiring a material document to be open

### Requirement: Material graph uses the shared authoring canvas
The desktop editor SHALL expose a Material Graph panel in the specialised-editor region. Its palette
SHALL be populated from the backend catalogue and its nodes, selection, layout, and links SHALL use
the shared `GraphCanvas` model rather than a material-specific graph implementation. Opening an
empty material canvas SHALL NOT fabricate semantic nodes.

The panel SHALL remain usable with its last compatible catalogue and authored nodes if the runtime
disconnects, while visibly reporting that backend compilation and preview services are unavailable.

#### Scenario: Backend node appears in the visible palette
- **WHEN** the backend catalogue contains a compatible node type unknown when the editor was built
- **THEN** the Material Graph panel SHALL show that node in its palette without an editor source change

#### Scenario: Adding a palette node uses the shared canvas
- **WHEN** an author selects a material node from the palette
- **THEN** the shared graph canvas SHALL create and display one node carrying the catalogue type and stable node identity

#### Scenario: Opening a mesh's graph source
- **WHEN** a selected mesh references a graph material with a project canvas source
- **THEN** the Material Graph panel SHALL reopen its nodes, properties, and links on the shared canvas
- **AND** an invalid source SHALL leave the current canvas intact and show a refusal

#### Scenario: Connecting visible pins edits the shared graph
- **WHEN** an author selects an output pin and then a compatible input pin
- **THEN** the panel SHALL add one shared-canvas link addressed by the stable node and pin identities
- **AND** the connection SHALL remain visible on subsequent frames without material-specific graph state

#### Scenario: Incompatible connection is refused visibly
- **WHEN** an author attempts to connect pins whose catalogue types are incompatible
- **THEN** the shared canvas SHALL remain unchanged
- **AND** the panel SHALL show the typed refusal without discarding the pending graph edits

#### Scenario: Canvas diagnostics navigate to their nodes
- **WHEN** the shared graph reports a node- or pin-precise preflight diagnostic
- **THEN** the Material Graph panel SHALL show its severity and pin identity
- **AND** selecting it SHALL select the responsible stable node

#### Scenario: Disconnect preserves the visible graph
- **WHEN** the runtime disconnects after nodes have been added
- **THEN** the Material Graph panel SHALL retain those nodes and SHALL report the backend as offline

#### Scenario: Validation and compilation remain backend operations
- **WHEN** an author requests validation or compilation from the Material Graph
- **THEN** the editor SHALL submit the current canvas through the live backend-service boundary with a request identity
- **AND** the editor SHALL NOT link material compiler, renderer, Metal, or VFX implementation code

#### Scenario: A terminal material result is correlated and visible
- **WHEN** the backend completes, fails, or cancels a material request
- **THEN** the Material Graph SHALL apply the result only to the matching request identity
- **AND** show either validation state, a stable compiled artefact identity, or the backend diagnostic

#### Scenario: Service loss preserves a pending authored canvas
- **WHEN** the runtime disconnects while a material request is pending
- **THEN** the request SHALL become a visible failure
- **AND** nodes, links, properties, layout, and the last terminal result SHALL remain available to the author

### Requirement: Material assets are assignable through the ordinary transaction path
The editor SHALL expose the material reference already owned by the engine's `MeshRenderer` as an
asset field. Assigning a material SHALL use the registered command and document transaction path,
SHALL preserve the mesh reference, and SHALL participate in undo, save, and reload. It SHALL use the
same `SetAssetReference` operation as mesh references so dependency tracking and future generic
reference rewriting do not require a material-specific path.

The editor SHALL reject attempts to assign a non-material source to the material field before
mutating the document.

#### Scenario: Material assignment preserves the mesh
- **WHEN** an author assigns a material asset to an imported mesh entity
- **THEN** the entity SHALL retain its mesh reference and gain the material reference in one undoable transaction

#### Scenario: Undo restores the previous material
- **WHEN** an author undoes a material assignment
- **THEN** the exact preceding material reference SHALL be restored without changing the mesh

### Requirement: Material graphs and object parameters remain editable and persistent
The Material Graph SHALL save a validated canvas through the engine's canonical graph writer,
preserve its editable canvas source, and keep invalid edits in memory without overwriting either
asset. A selected object's declared graph parameters SHALL appear as typed, undoable Inspector
fields and SHALL be saved with the scene. Graph saves SHALL preserve existing object overrides.

#### Scenario: Save and reopen a graph
- **WHEN** the engine accepts the authored canvas
- **THEN** the editor SHALL save `.cygraph` and `.cymatcanvas` beside one another
- **AND** reopening the graph SHALL restore nodes, properties, and links

#### Scenario: Edit an object parameter
- **WHEN** an author changes a declared graph parameter in the Inspector
- **THEN** the scene SHALL store that object's override in an undoable transaction
- **AND** a supported Diffuse colour override SHALL appear in the authored viewport

#### Scenario: Preview an unsaved graph colour
- **WHEN** an author changes the colour of a supported graph assigned to an authored mesh
- **THEN** the editor SHALL submit the current canvas through the material service without saving either graph file
- **AND** the hosted viewport SHALL display the new colour after the matching validated request
- **AND** an invalid or unsupported graph SHALL leave the last valid preview intact and report a diagnostic
- **AND** an explicit per-object colour override SHALL continue to take precedence
