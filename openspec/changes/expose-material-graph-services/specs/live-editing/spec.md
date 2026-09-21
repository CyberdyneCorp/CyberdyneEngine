# live-editing Spec Delta

## ADDED Requirements

### Requirement: Editor service envelopes on the live bridge
The live bridge SHALL carry the same request and event envelopes defined by
`editor-backend-services`, including schema identity and version, request identity, operation,
payload, progress, cancellation, and terminal outcome.

Existing specialised live messages SHALL retain their tags for compatibility. Capability
negotiation SHALL report supported envelope schemas and operations independently of the ABI version
used by the connection handshake.

#### Scenario: Hosted compilation is asynchronous
- **WHEN** an editor submits a material compile request to a hosted runtime
- **THEN** the send SHALL return immediately and progress and completion SHALL arrive as service events

#### Scenario: Cancellation crosses the bridge
- **WHEN** the editor cancels an in-flight request
- **THEN** the runtime SHALL receive the original request identity and return one honest terminal outcome

