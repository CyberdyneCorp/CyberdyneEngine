# native-abi Spec Delta

## ADDED Requirements

### Requirement: Asynchronous editor service interface
The append-only C interface SHALL expose the `editor-backend-services` contract through opaque
session handles and POD request and event envelopes. It SHALL provide entries to open and close a
session, submit a request, cancel a request, and poll events without blocking.

Envelope payloads SHALL be byte spans owned according to explicit lifetime rules. Callers SHALL NOT
receive C++ objects, implementation pointers, callbacks into renderer code, or backend-specific
structures.

#### Scenario: Poll never blocks
- **WHEN** an editor-service session has no event ready
- **THEN** polling SHALL return immediately with `has_event` false and SHALL NOT block the editor thread

#### Scenario: Older client ignores appended entries
- **WHEN** a client built before editor services loads a newer engine
- **THEN** its shorter interface table SHALL remain valid and its existing calls SHALL behave unchanged
