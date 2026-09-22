# Editor backend services

`cy::editor-backend` is the engine-owned implementation behind the editor's C ABI and live
protocol. The editor sends stable operation names and versioned byte payloads; it never links the
material compiler, renderer, Metal, or VFX implementation.

The first vertical slice supports:

- `capabilities.get` — operation discovery and first-slice target feature bits;
- `material.catalogue.get` — deterministic schema-2 envelope/catalogue-version-3 node, pin and
  typed-property catalogue with manifest identities, typed constraints, enum choices, asset-kind
  filters, semantic/stage/domain metadata and required target-capability bits. Schema 1 remains
  readable by the editor for compatibility;
- `material.validate` — parses and lowers canonical CyberGraph input;
- `material.compile` — compiles the material family and returns its cook identity, source graph
  dependency identity, program count, and stable texture-asset dependency identities;
- `preview.create`, `preview.destroy`, `preview.parameter.update`, and `preview.reload` — isolated
  generational handles, idempotent destruction, stale-handle diagnostics, exact entity/material-slot
  target acknowledgements, and typed bool/integer/float/vector/texture parameter updates bound to an
  applied artefact generation.

Requests are copied at submission, identified by nonzero request IDs, cancelled cooperatively, and
publish exactly one terminal event. Payload schemas are versioned independently of ABI 1.2 and of
the live message framing.
