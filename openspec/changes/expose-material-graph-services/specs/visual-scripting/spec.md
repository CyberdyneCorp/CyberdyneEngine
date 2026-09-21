# visual-scripting Spec Delta

## MODIFIED Requirements

### Requirement: Stable graph identity
Node instances, node-type definitions, and pin definitions SHALL carry **stable identifiers** that
survive editing and renaming, using the identity mechanism in `core-type-system`.

A node instance SHALL retain its author-owned `NodeKey`. Each registered node type SHALL carry a
manifest-backed `NodeTypeId`, and each pin SHALL carry a manifest-backed `PinId` unique within that
node type. Names SHALL remain searchable display and migration metadata and SHALL NOT be serialized
as the only identity of a type, pin, link, diagnostic location, or breakpoint.

Visual layout — positions, comments, collapsed state — SHALL be stored **separately from semantics**,
so that moving a node is not a semantic change.

Stable identity SHALL support: debugging and breakpoints, semantic diffing, three-way merging, hot
reload state migration, and node type version migration. Removed node types and pins SHALL leave
tombstones and their identifiers SHALL NOT be reused.

#### Scenario: Moving a node changes nothing
- **WHEN** a node is repositioned
- **THEN** the semantic representation SHALL be unchanged and the compiled program identical

#### Scenario: A breakpoint survives an edit
- **WHEN** a graph is edited around a node carrying a breakpoint
- **THEN** the breakpoint SHALL remain on that node

#### Scenario: A pin is renamed
- **WHEN** a pin's display name changes while its `PinId` remains unchanged
- **THEN** existing links, diagnostics, and breakpoints SHALL continue to address that pin

#### Scenario: Removed identity is not reused
- **WHEN** a pin is removed and another pin is added later
- **THEN** the new pin SHALL receive a fresh identity and the removed pin SHALL remain tombstoned

## ADDED Requirements

### Requirement: Versioned node catalogue
Each graph domain SHALL expose a versioned catalogue of node types through
`editor-backend-services`. A node definition SHALL include stable type and pin identities, revision,
owning module, typed properties, typed defaults, constraints, declared conversions, capability
requirements, determinism, purity, and authoring metadata needed to render a palette and inspector.

Catalogue ordering and encoding SHALL be deterministic. A catalogue revision SHALL change whenever
any observable definition changes.

#### Scenario: Editor constructs a property control from data
- **WHEN** a node property declares a floating-point default, range, step, unit, and tooltip
- **THEN** the editor SHALL construct its control without node-specific editor code

#### Scenario: Catalogue drift is visible
- **WHEN** a plugin changes a pin constraint or default
- **THEN** the catalogue revision SHALL change and cached clients SHALL refresh it

