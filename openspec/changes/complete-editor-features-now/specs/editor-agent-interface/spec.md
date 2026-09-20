# Spec Delta

## ADDED Requirements

### Requirement: Desktop agent sessions remain under human control
The desktop editor SHALL be able to host its MCP agent interface while its window remains usable.
The editor SHALL show each connected session's identity, stated intent, scope, budget state, and
current operation, and SHALL let the human pause or revoke the session without restarting the
editor.

Revocation SHALL prevent new operations immediately and SHALL abandon an in-flight uncommitted
transaction. An irreversible or external operation SHALL be presented to the human in the desktop
editor for explicit confirmation unless the session holds a deliberate, time-bounded grant for that
effect class.

#### Scenario: Agent and human work concurrently
- **WHEN** an MCP client performs a long authoring operation while the desktop window is open
- **THEN** the human SHALL remain able to inspect and edit the project, observe the operation, and
  pause or revoke the session

#### Scenario: Revocation abandons incomplete work
- **WHEN** the human revokes an agent session during an uncommitted transaction
- **THEN** the transaction SHALL be abandoned, no partial persistent edit SHALL remain, and later
  requests from that session SHALL be refused as revoked

#### Scenario: Destructive request reaches the human
- **WHEN** an agent requests an irreversible operation without a matching temporary grant
- **THEN** the desktop editor SHALL state what will happen and what will be lost before accepting or
  refusing the request
