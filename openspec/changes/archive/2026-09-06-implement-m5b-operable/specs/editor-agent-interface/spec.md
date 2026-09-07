## MODIFIED Requirements

### Requirement: Agents are editor clients, not a privileged peer
The editor SHALL expose an **agent interface**: a connection over which an autonomous agent reads
project state, invokes editor actions, and observes the result.

An agent SHALL have **no capability a human user does not have**, and SHALL reach every capability
through the same mechanisms — the command registry, the transaction system, the selection service,
and the engine's own renderer. There SHALL be no agent-only mutation path, no agent-only query path,
and no bypass of a check a human interaction is subject to.

The interface SHALL be optional at build time and absent from a shipped runtime.

**Authoring is the capability, not querying.** The interface SHALL support the full loop an author
performs: compose a scene by creating, placing and manipulating objects; **create and edit project
source**, including gameplay scripts; trigger the build and module reload that makes an edit live;
enter and leave play mode; and **observe the rendered result** in order to decide what to do next.

The loop is the point. An agent that can read a project and answer questions about it is a search
tool. An agent that can change something and then *see whether the change did what it intended* is
a collaborator, and the difference is entirely the observation step.

Creating source is subject to every rule that governs any other mutation: it is a transaction, it is
attributed, it is undoable where the file system permits and confirmed where it does not, and it is
refused outside the connection's declared scope.

#### Scenario: An agent composes and verifies
- **WHEN** an agent creates entities, writes a gameplay script, reloads it and enters play mode
- **THEN** each step SHALL go through the same commands and transactions a human would use, and the
  agent SHALL be able to observe the rendered result

#### Scenario: Writing source is not a special case
- **WHEN** an agent creates or edits a script in the project
- **THEN** it SHALL be a transaction with an actor and an intent, subject to scope and effect class
  like any other change

#### Scenario: An agent cannot do what a person cannot
- **WHEN** an agent attempts an operation
- **THEN** it SHALL succeed exactly when the same operation invoked from the command palette would
  succeed, and fail with the same reason when it would not

#### Scenario: The interface is removable
- **WHEN** the editor is built without the agent interface
- **THEN** no agent transport SHALL be compiled, linked, or listening
