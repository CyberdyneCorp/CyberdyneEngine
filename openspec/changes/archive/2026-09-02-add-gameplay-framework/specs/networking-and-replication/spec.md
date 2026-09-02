## MODIFIED Requirements

### Requirement: Remote procedure calls
RPCs SHALL be declared with: a direction (`ToServer`, `ToClients`, `ToOwner`, `ToTarget`), a
delivery mode, and an authority requirement.

RPC parameters SHALL be typed and serialized with the same machinery as replicated fields.

**RPCs SHALL NOT be the channel for gameplay intent.** Client-to-server gameplay intent travels as
**gameplay commands** (see `gameplay-framework`), which carry prediction, structural authority
validation, and replay recording. RPCs carry what is not gameplay intent: chat, session control,
notifications, and out-of-band requests.

Intent sent by RPC would bypass prediction, replay, and command validation; the boundary is
therefore specified rather than left to preference, and development builds SHOULD report an RPC
that appears to carry gameplay intent.

The engine SHALL validate on receipt: that the sender is permitted to invoke this RPC on this
entity, and that parameters are within declared bounds.

#### Scenario: Unauthorised RPC
- **WHEN** a client invokes a server-only RPC it is not permitted to call
- **THEN** the server SHALL reject it, log it, and optionally apply a rate-limit or disconnect
  policy

#### Scenario: RPC ordering with state
- **WHEN** an RPC depends on state replicated in the same tick
- **THEN** the ordering guarantee SHALL be documented, and the RPC SHALL be delivered after that
  tick's state is applied

#### Scenario: Intent uses the command channel
- **WHEN** a client asks the server to perform a gameplay action
- **THEN** it SHALL be a gameplay command, so prediction, validation, and replay apply
