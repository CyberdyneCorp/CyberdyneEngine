# Spec Delta

## ADDED Requirements

### Requirement: Swift Workspace
The editor SHALL provide a Swift Workspace for project gameplay sources with a project-relative file
tree, tabbed text editing, source diagnostics, symbol navigation, and build and development-module
reload actions.

The source workspace SHALL remain compatible with the ordinary Swift package and its external tools.
An edit made through the workspace, an external editor, or an agent SHALL converge on the same file
state; the editor SHALL detect an external change and SHALL NOT silently overwrite an unsaved buffer.

#### Scenario: Edit build and reload
- **WHEN** a developer edits a Swift gameplay source, saves it, and requests build and reload
- **THEN** the editor SHALL show structured build progress and diagnostics and SHALL report whether
  the development module reloaded with state preserved

#### Scenario: Diagnostic navigation
- **WHEN** a Swift diagnostic refers to a project source location
- **THEN** selecting it SHALL open the source at the referenced line and column

#### Scenario: Agent edit does not overwrite a human buffer
- **WHEN** an agent or external tool changes a source file that has unsaved desktop edits
- **THEN** the editor SHALL surface the conflict and require an explicit reload, keep, or merge
  decision rather than choosing one version silently

### Requirement: Service-backed operational panels
Settings, source control, and transaction history SHALL be available as desktop panels backed by the
same services used by commands and agents. Views SHALL NOT invoke provider processes, rewrite project
files, or own authoritative history directly.

The Settings panel SHALL distinguish project settings from user preferences. The Source Control panel
SHALL expose only operations the selected provider declares. The History panel SHALL show the
transaction name, actor, intent where present, cursor position, and truncation state.

#### Scenario: Unsupported source-control action stays unavailable
- **WHEN** the active provider does not support exclusive locking
- **THEN** the Source Control panel SHALL show locking as unavailable with the provider's reason and
  SHALL NOT emulate success

#### Scenario: History identifies an agent edit
- **WHEN** a transaction was produced by an agent session
- **THEN** the History panel SHALL show the agent, session, and stated intent recorded by the
  transaction

#### Scenario: User preference stays out of project settings
- **WHEN** a user changes an interface preference in the Settings panel
- **THEN** the project settings file SHALL remain unchanged

### Requirement: Complete local hierarchy authoring
The hierarchy SHALL support additive, subtractive, and range selection; inline transactional rename;
and drag-and-drop reparenting through registered commands. Reparenting SHALL refuse cycles and SHALL
preserve stable identity.

Controls for visibility, locking, prefab state, or unloaded-world records SHALL appear only when the
document model can represent and enforce them; the editor SHALL NOT display controls that pretend to
change unsupported state.

#### Scenario: Reparenting is one undoable edit
- **WHEN** a user drags a selected node onto a valid new parent
- **THEN** the hierarchy SHALL perform one transaction, preserve the node's identity, and undo SHALL
  restore its previous parent and order

#### Scenario: Reparenting cycle is refused
- **WHEN** a user attempts to reparent a node beneath one of its descendants
- **THEN** the operation SHALL be refused with the cycle identified and the document SHALL remain
  unchanged
