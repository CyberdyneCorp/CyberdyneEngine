# Editor backend services

`cy::editor-backend` is the engine-owned implementation behind the editor's C ABI and live
protocol. The editor sends stable operation names and versioned byte payloads; it never links the
material compiler, renderer, Metal, or VFX implementation.

The first vertical slice supports:

- `capabilities.get` — operation discovery and first-slice target feature bits;
- `material.catalogue.get` — deterministic schema-3 envelope/catalogue-version-5 node, pin and
  typed-property catalogue with manifest identities, typed constraints, enum choices, asset-kind
  filters, semantic/stage/domain metadata, required target-capability bits, and engine-owned node
  stage masks (`surface`, `vertex`, or both). Schemas 1 and 2 remain readable by the editor;
- `vfx.catalogue.get` (when `CY_VFX` is enabled) — schema-2 catalogue generated from the VFX
  compiler's registered node types, typed pins, property descriptors, and registered data-interface
  names for sample controls. The editor requests it after the material
  catalogue and opens the VFX shared canvas only when a compatible nonempty catalogue arrives.
  Runtime preview is subsequent issue #15 work tracked in
  `openspec/changes/implement-issue-15-graph-authoring/`;
- `vfx.authoring-capabilities.get` (when `CY_VFX` is enabled) — renderer kinds and named
  compositing refusals from the VFX renderer, CPU/GPU target compile and runtime availability
  from the VFX runtime, and registered built-in interface names with supported simulation paths.
  Its schema-2 payload reports no GPU preview device until one is attached; the editor reader
  also accepts schema 1;
- `vfx.compile` (when `CY_VFX` is enabled) — reads an editable `.cyvfxdoc` through the engine,
  resolves the compiler node registry, and compiles the system. Schema 1 returns the cook key,
  kernel counts, per-emitter memory and cost reports, derived attribute layouts, and generated
  Slang. Failed requests use diagnostic schema 2 with node, pin, emitter index, and stage identity;
  the reader also accepts unscoped schema 1 diagnostics. A `cyvfxbundle 1` request may include
  explicit project module sources, which the engine validates and composes before cooking;
- `material.validate` — parses and lowers canonical CyberGraph input;
- `material.author` — validates a canvas and returns engine-canonical `.cygraph` text;
- `material.preview.set` — validates an unsaved canvas and applies its canonical graph to a
  runtime-owned authored scene. Its schema-1 payload is two little-endian, length-prefixed UTF-8
  strings: project-relative `.cygraph` reference followed by `cymatcanvas` source;
- `material.compile` — compiles the material family and returns its cook identity, source graph
  dependency identity, program count, and stable texture-asset dependency identities;
- `preview.create`, `preview.destroy`, `preview.parameter.update`, and `preview.reload` — isolated
  generational handles, idempotent destruction, stale-handle diagnostics, exact entity/material-slot
  target acknowledgements, and typed bool/integer/float/vector/texture parameter updates bound to an
  applied artefact generation.

Requests are copied at submission, identified by nonzero request IDs, cancelled cooperatively, and
publish exactly one terminal event. Payload schemas are versioned independently of ABI 1.2 and of
the live message framing.

Preview operations may be connected to a runtime-owned `MaterialPreviewRuntime`. Compiled
materials are published to that interface before their identities are returned, and create,
reload, parameter-update and destroy acknowledgements are emitted only after the runtime accepts
the operation. The interface carries engine-owned compiled material and stable binding data; no
renderer, Metal or compiler type crosses the C ABI/live protocol or enters the Rust editor.
Hosts without that interface reject `preview.create` with `preview-runtime-unavailable` and omit
the preview feature bit rather than reporting a protocol-only echo as a visible reload.
`material.preview.set` uses a separate `MaterialAuthoringRuntime` host seam; it never writes the
graph asset. Hosts without an authored scene reject it with `material.preview.unavailable`.
