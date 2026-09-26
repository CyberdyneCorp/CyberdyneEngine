# editor-architecture Spec Delta

## ADDED Requirements

### Requirement: VFX and vertex materials share authoring infrastructure
VFX stage graphs and material vertex graphs SHALL use the existing shared node canvas, stable node and pin identities, document transactions, and undo/redo. Commands available in the editor SHALL have equivalent MCP operations. Saving and reopening SHALL preserve editable graph state and runtime parameters.

#### Scenario: Agent and human edits agree
- **WHEN** a human and an MCP client perform equivalent node insertion, connection, and parameter edits
- **THEN** they SHALL produce equivalent authored documents and undo records
