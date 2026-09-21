# Design

## Context

See `proposal.md` for motivation. CyberGraph already owns graph validation and domain registration;
the material compiler already owns lowering, optimisation, reports, preview compilation and cook
keys; the build service already owns immutable content identities and dependency records. The Rust
live session is asynchronous, but its message vocabulary is closed and the stable C ABI exposes only
world and component operations. The implementation must join these existing answers without moving
compiler or renderer types into either boundary.

## Goals / Non-Goals

**Goals:**

- Establish one service model whose envelopes have byte-identical payload semantics over C ABI and
  live transport.
- Make catalogue and diagnostic data self-describing, stable, deterministic, and usable without
  linking compiler code into the editor.
- Reuse the existing material lowering/compiler/shader/cook paths.
- Preserve append-only ABI and live-message compatibility.

**Non-Goals:**

- VFX catalogue and compilation operations; they are the next consumer of this contract.
- General build/package/deploy, profiler/debugger, spatial query, or remote viewport services.
- A second graph serialization format or a Rust material compiler.
- Blocking convenience APIs on the editor interface thread.

## Decisions

### 1. A common binary envelope, not one ABI entry per operation

Both transports carry `schema`, `schema_version`, `request`, `operation`, and an opaque payload.
Events additionally carry `kind` and terminal status. The C ABI provides session lifecycle,
submit/cancel/poll; the live protocol adds request/event/cancel variants with the same bytes.

This keeps the append-only ABI small and lets operation schemas evolve without adding an ABI entry
for every catalogue query. Individual typed ABI functions were rejected because they would duplicate
the live protocol's evolution and force compiler concepts into the base ABI header.

### 2. The service host is registered beneath the ABI

The ABI module owns only session and envelope mechanics. A runtime or tool registers an
`EditorServiceBackend` at editor initialisation. The material-aware implementation lives at the
rendering/tool layer and depends downward on CyberGraph and material compiler code; the ABI never
depends upward. A missing backend returns an unsupported capability set rather than linking one.

### 3. Persistent node definition identities use the identity manifest

`NodeTypeId` is globally persistent and `PinId` is persistent within a node type. Existing names
remain lookup metadata during migration, and existing `NodeKey` remains the graph-instance identity.
Links gain pin IDs while the version-1 text reader resolves names through the catalogue during
migration. The next graph format writes IDs and optional names for readable diffs.

Name hashes without manifest records were rejected because renames would silently change identity,
contrary to `core-type-system` and `visual-scripting`.

### 4. Catalogue snapshots are deterministic immutable payloads

A snapshot contains a catalogue ID, monotonically changing schema revision, semantic digest, node
definitions, pin definitions, property definitions, conversions and target support. Definitions are
sorted by persistent identity before encoding. Defaults and constraints use the boundary value
vocabulary, not C++ unions or Rust enums private to a client.

### 5. Diagnostics use one location algebra

A location is one of source range, asset, graph node/pin, generated source range, or backend stage.
Related locations preserve full material-to-backend lineage. Existing graph, material and shader
diagnostics are adapted at the service edge; their internal representations remain unchanged.

### 6. Content identity and derivation identity remain separate

The compiler's cook key becomes derivation/provenance data. Emitted bytes are placed in the existing
artefact store to obtain content hashes. Results return both and list direct inputs plus generated
children. Treating the cook key as a content hash was rejected because equal bytes can be produced by
distinct derivations and because the cache already distinguishes the two.

### 7. Cancellation is cooperative and publication is the commit point

Each accepted request owns a cancellation token. Compilation checks it between validation,
lowering, optimisation, shader compilation and publication. Artefact publication is atomic and is the
point after which cancellation cannot truthfully win. The dispatcher serialises the terminal event,
so racing cancel and completion cannot settle twice.

### 8. Preview worlds are leased runtime objects

A preview handle is generational and scoped to one service session. Destroy is idempotent. Session
loss destroys all its previews. A preview owns runtime world state, artefact bindings and viewport
publication, while authored graph state remains in the editor document.

## Risks / Trade-offs

- **Large catalogues create control-message pressure** → Return immutable snapshots with digests and
  an unchanged response; retain the existing bulk-transport escape hatch.
- **Identity migration can break old name-only graphs** → Add manifest aliases and dual-read tests
  before changing the canonical writer.
- **Cancellation cannot interrupt every backend compiler call** → Report progress boundaries and
  complete honestly if the non-cancellable call reaches publication.
- **A generic envelope can become an untyped dumping ground** → Every operation references a named,
  versioned schema with independent round-trip and compatibility tests.
- **Copied Rust catalogues may survive accidentally** → Add a dependency/source gate and a contract
  test that adds a backend-only catalogue entry and observes it in Rust.

## Migration Plan

1. Introduce identities, envelope codecs and tests without changing existing message tags or graph
   writing.
2. Append ABI 1.2 service entries and regenerate descriptions/SDK bindings.
3. Add live service messages and cross-language byte fixtures.
4. Publish the engine-owned material catalogue and migrate the Rust material editor to it.
5. Add validation, compilation and artefact operations, then preview lifecycle and runtime updates.
6. Remove the copied Rust material tables only after parity and end-to-end tests pass.

Rollback is additive: older clients ignore appended ABI entries and never send new live-message tags;
the previous material command-line path remains usable until the migration gate closes.
