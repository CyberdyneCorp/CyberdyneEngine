# editor-backend-services Specification

## Purpose

Defines the stable, transport-neutral services through which editors and tools discover authoring
schemas, validate and compile content, manage previews, update runtime data, and inspect results.

## ADDED Requirements

### Requirement: One service contract across transports
Editor backend operations SHALL have one semantic request and result contract carried through the
stable C ABI for embedded clients and through the live bridge for hosted or remote clients.

The contract SHALL expose no renderer, shader compiler, graphics API, or VFX implementation type.
Equivalent requests through either transport SHALL produce equivalent typed results.

#### Scenario: Locality changes no operation
- **WHEN** a material catalogue request is issued through the C ABI and through the live bridge
- **THEN** both results SHALL use the same schema and stable identities

#### Scenario: Editor links no implementation internals
- **WHEN** the editor dependency graph is inspected
- **THEN** it SHALL contain no renderer, shader compiler, Metal, or VFX implementation target

### Requirement: Asynchronous request lifecycle
Every operation SHALL carry a non-zero request identifier unique within its session and SHALL
complete asynchronously. Long operations SHALL emit structured progress and SHALL accept a
cancellation request naming the original request identifier.

Every accepted request SHALL produce exactly one terminal outcome: `Completed`, `Failed`,
`Cancelled`, or `Superseded`. Cancellation SHALL be best effort; if work has crossed its publication
point, the service SHALL complete it normally rather than claim it was cancelled.

#### Scenario: Cancellation settles once
- **WHEN** a client cancels a material compilation before publication
- **THEN** the request SHALL finish once as `Cancelled` and SHALL publish no compiled artefact

#### Scenario: Late cancellation is honest
- **WHEN** cancellation arrives after an artefact was atomically published
- **THEN** the request SHALL finish as `Completed` and return that artefact rather than report a false cancellation

### Requirement: Independently versioned schemas
Every request and event payload SHALL identify its schema and schema version independently of the
ABI and transport version. A receiver SHALL reject an unsupported required schema before interpreting
its payload and SHALL preserve unknown optional fields when relaying an envelope.

#### Scenario: Unknown schema is refused
- **WHEN** a client submits a required schema version newer than the service supports
- **THEN** the request SHALL fail with a version diagnostic naming the supported range

### Requirement: Stable service identities
Persistent identities in service payloads SHALL be fixed-width values or canonical textual forms
whose meaning is stable across processes, builds, platforms, transports, and compiler versions.
Names SHALL be metadata and SHALL NOT be used as persistent identity.

#### Scenario: Rename preserves a saved graph
- **WHEN** a node type or pin is renamed without changing its manifest identity
- **THEN** an older graph SHALL resolve to the renamed definition without rewriting its connections

### Requirement: Structured diagnostics
Diagnostics SHALL carry severity, stable code, message, primary location, zero or more related
locations, and optional remedy. A graph location SHALL identify the asset or document, node instance,
and pin definition where applicable.

Diagnostics SHALL be machine-readable, order-stable, and navigable by the editor.

#### Scenario: Validation points to a pin
- **WHEN** a connection has an incompatible type
- **THEN** the diagnostic SHALL name both endpoint identities and types and make the target pin navigable

#### Scenario: Diagnostic schema evolves independently
- **WHEN** a material operation returns a structured diagnostic list to an editor that also reads the legacy single-diagnostic payload
- **THEN** the payload's own schema version SHALL select the decoder without changing the ABI or live-message envelope
- **AND** diagnostics SHALL retain their deterministic validation order

### Requirement: Artefact and dependency results
Compilation results SHALL distinguish the immutable content identity from the derivation identity
that records how it was produced. They SHALL include producer and schema versions, target and feature
profile, direct dependencies, generated debug maps, and the identities of emitted artefacts.

#### Scenario: Same bytes and different derivations remain distinguishable
- **WHEN** two valid derivations happen to emit identical bytes
- **THEN** their content identities MAY match while their derivation identities and provenance remain distinct

### Requirement: Capability discovery
A client SHALL be able to query service, target, node, and feature capabilities before submitting
work. Capability results SHALL state support, limits, required features, declared degradation, and a
structured reason when unsupported.

#### Scenario: Unsupported node is known before compile
- **WHEN** a mobile target does not support a material node
- **THEN** catalogue or capability data SHALL mark it unsupported with the same reason compilation would report

### Requirement: Isolated preview lifecycle
The service SHALL create and destroy runtime-owned preview worlds identified by stable session
handles. Each preview SHALL be isolated from authoring and play worlds, own its transient resources,
and be destroyed idempotently.

#### Scenario: Closing an asset editor releases its preview
- **WHEN** the client destroys a material preview world twice
- **THEN** the first request SHALL release its resources and the second SHALL succeed as an idempotent no-op

### Requirement: Runtime updates and reload acknowledgement
Runtime parameter updates SHALL address a preview or runtime instance, compiled artefact generation,
and parameter definition identity with a typed value. The acknowledgement SHALL report the observed
generation and whether the update was applied as data, required recompilation, restarted state, was
superseded, or was rejected.

#### Scenario: Runtime data changes without compilation
- **WHEN** a runtime-classified material parameter receives a compatible value
- **THEN** the acknowledgement SHALL report `Applied` and no compilation request SHALL be created
