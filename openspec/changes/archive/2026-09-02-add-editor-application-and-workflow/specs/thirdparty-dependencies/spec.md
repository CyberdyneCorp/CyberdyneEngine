## ADDED Requirements

### Requirement: Rust editor dependencies
The editor's Rust dependencies SHALL be governed by the same policy as the engine's C++ dependencies:
declared, pinned, licence-reviewed, vendored or reproducibly acquired, and justified.

The Rust ecosystem's low cost of adding a dependency SHALL NOT be treated as a reason to add them
freely. The editor SHALL prefer a small, audited set, and SHALL account for **transitive** dependency
count in the decision, since that is where the cost actually accrues.

The **interface toolkit** SHALL be an integrated dependency selected on measurement, kept behind the
editor's own abstractions as required by `editor-rust-application`, and replaceable. It SHALL NOT
appear in plugin-facing or protocol interfaces.

The editor SHALL build itself, rather than integrate, the following: its document and transaction
model, its command and service architecture, its view model layer, its engine SDK, its protocol
client, and its viewport transport. These are where the editor's behaviour is decided.

A Rust dependency SHALL be evaluated for maintenance status, licence, `unsafe` usage, build time
cost, and whether it can be replaced without changing editor architecture.

#### Scenario: The toolkit is an integration, not an architecture
- **WHEN** the interface toolkit is evaluated
- **THEN** it SHALL be judged replaceable behind editor abstractions, and a toolkit that would own
  the editor's state or command model SHALL be rejected

#### Scenario: Transitive cost counts
- **WHEN** a crate is proposed
- **THEN** its transitive dependency set SHALL be part of the evaluation, not only the crate itself

#### Scenario: Reproducible acquisition
- **WHEN** the editor is built from a clean checkout
- **THEN** its dependency set SHALL be reproducible from pinned versions, as for the engine
