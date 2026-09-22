# Proposal

## Why

The material compiler, graph validator, artefact cache, Metal shader path, and live bridge exist, but
the editor cannot compose them without copied catalogues, command-line intermediates, or direct
knowledge of engine internals.  The missing product is a stable editor-service contract: one
versioned, asynchronous surface that works through both the C ABI and the live protocol.

## What Changes

- Add a transport-neutral editor backend service contract with request identifiers, progress,
  cancellation, exactly one terminal result, independently versioned payload schemas, and
  capability negotiation.
- Give graph node types and pins manifest-backed persistent identifiers while retaining names as
  display and migration metadata and `NodeKey` as the stable identity of a node instance.
- Expose a versioned material node catalogue containing typed pins, properties, defaults,
  constraints, conversions, capabilities, and target availability.
- Expose material graph validation and compilation, including structured diagnostics, lowering
  inspection, content and derivation identities, dependency information, and compiled artefact
  metadata.
- Expose isolated preview-world creation/destruction, typed runtime parameter updates, and reload
  acknowledgements without exposing renderer, shader compiler, Metal, or VFX implementation types.
- Replace the Rust editor's copied material catalogue with catalogue data obtained through the
  public service contract.
- Move the material authoring front-end responsibility currently recorded in M11.e tasks 5a.1–5a.3
  into this change; M11.e retains packaging, deployment, and final shipping integration.

## Capabilities

### New Capabilities

- `editor-backend-services`: The transport-neutral request, result, schema, catalogue, artefact,
  capability, preview, and reload contract consumed by editor and command-line clients.

### Modified Capabilities

- `visual-scripting`: Node-type and pin definitions receive persistent identities and catalogue
  metadata suitable for tools.
- `native-abi`: The append-only interface gains the generic asynchronous editor-service boundary.
- `live-editing`: The live bridge carries the same service envelopes, cancellation, capability
  negotiation, preview lifecycle, parameter updates, and reload acknowledgements.
- `material-compiler`: Material catalogue, validation, compilation, inspection, dependencies, and
  artefact metadata become available through the editor-service contract.
- `editor-architecture`: Material tooling consumes the public service contract and runtime-owned
  preview worlds instead of copied compiler or renderer knowledge.

## Impact

- Affected engine areas: CyberGraph identity and registration, material lowering/compiler/cook,
  runtime editor bridge, preview-world hosting, the stable C ABI, and generated SDK descriptions.
- Affected editor areas: protocol messages, runtime session services, graph catalogue, material
  specialised editor, diagnostics and operation progress.
- Existing ABI entries remain unchanged; new entries are append-only and require an ABI minor
  version increase. Existing live messages retain their tags.
- No new third-party dependency is introduced. The editor still links no renderer, shader compiler,
  Metal, or VFX target.
