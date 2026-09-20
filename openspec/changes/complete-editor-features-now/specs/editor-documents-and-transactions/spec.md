# Spec Delta

## ADDED Requirements

### Requirement: Document tabs expose workspace lifecycle
Every open document SHALL have a visible tab showing its display name, active state, and dirty state.
Activating a tab SHALL restore that document's selection and view state without dirtying content.

Closing a dirty document SHALL require Save, Discard, or Cancel. Cancel SHALL preserve the document
and its history; Discard SHALL be explicit; Save SHALL complete successfully before the document is
removed. Closing the active document SHALL select a deterministic remaining document.

#### Scenario: Switching tabs does not edit content
- **WHEN** a user activates another document tab
- **THEN** the target document's view state SHALL be restored and neither document SHALL gain a
  transaction or become dirty

#### Scenario: Dirty close can be cancelled
- **WHEN** a user closes a dirty document and chooses Cancel
- **THEN** the document SHALL remain open with its content, dirty state, selection, and history
  unchanged

#### Scenario: Save failure prevents close
- **WHEN** saving a dirty document fails during close
- **THEN** the document SHALL remain open and the failure SHALL state its cause and remedy

### Requirement: Semantic comparison is operable
The editor SHALL present semantic differences and three-way merge conflicts using document, entity,
component, field, graph, and override identities rather than textual hunks. A user SHALL be able to
accept either side of a conflict or provide a new value, and the resolved merge SHALL be committed as
a transaction.

#### Scenario: Independent changes merge automatically
- **WHEN** the local and incoming revisions modify different fields from the same base
- **THEN** the comparison SHALL merge both changes without asking the user to resolve a conflict

#### Scenario: Conflicting field values require a decision
- **WHEN** the local and incoming revisions assign different values to the same field
- **THEN** the editor SHALL show both values and SHALL NOT produce a merged document until the
  conflict is explicitly resolved
