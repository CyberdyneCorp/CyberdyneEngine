## MODIFIED Requirements

### Requirement: Transactions are the only path for persistent mutation
Every mutation of persistent project state SHALL be recorded as a **transaction** against a
document. Tools, panels, gizmos, importers, and plugins SHALL have no other write path.

A transaction SHALL carry a user-facing name, an ordered list of typed operations, and the document
it applies to.

A tool that mutates state outside a transaction SHALL be a defect, and development builds SHALL
detect and report it.

Widgets and tools SHALL NOT be the authoritative owner of project state; they read from and write
through documents.

Every transaction SHALL record the **actor** that produced it — the human user, or the agent,
session and stated intent defined in `editor-agent-interface`. Attribution SHALL be visible wherever
history is: the undo stack, the journal, and semantic diff.

This is not a security control and SHALL NOT be treated as one. It answers "who changed this, and
what were they trying to do" — a question every collaborator already has, and which an unattributed
history cannot answer at all.

#### Scenario: History names its authors
- **WHEN** a document's history is inspected
- **THEN** each transaction SHALL name the actor that produced it

#### Scenario: A plugin edit is undoable automatically
- **WHEN** a plugin modifies a document through the editor API
- **THEN** its changes SHALL be recorded as a transaction and SHALL be undoable without the plugin
  implementing undo

#### Scenario: Out-of-band mutation is caught
- **WHEN** development builds detect state changed outside a transaction
- **THEN** it SHALL be reported naming the document and the mutating code
