# Design

## Shared graph authoring

Use `cy_editor_interface::specialised::GraphCanvas` for both domains. The engine service publishes versioned catalogues, including stable identities, typed pins, properties, capabilities, and stage metadata. The editor stores node instances and layout separately; graph validation and compilation remain in the engine. A catalogue addition must reach the palette without editing a Rust node list.

## VFX asset and compilation path

The authored hierarchy is system → emitters → spawn/update/event/render stage graphs. Modules are separately saved graph assets referenced by stages. Each emitter declares its CPU or GPU target, renderer, user parameters, and data-interface bindings. Editor commands produce document transactions, and the same commands back MCP. The service compiles through `cy::vfx-compiler`, returning `CompileReport`, attribute layout, generated source on demand, and node-located diagnostics. Save writes a reopenable authoring document and canonical cook input. Runtime parameter updates use the live instance path; graph edits schedule compilation.

The editable draft is a versioned `.cyvfxdoc` project source. `vfx.document.save` journals its prior and new contents in the active scene document so undo/redo restores the file; `vfx.document.read` reopens it through the same command registry. The engine service will produce canonical cook input from this document after stage validation and compilation are connected. The draft format is not itself a cooked effect.

Reusable modules have a separate versioned `.cyvfxmodule` source contract: compatible stage, typed host inputs, dependency names, and shared-canvas graph. The editor can encode and reopen the asset, and the engine has an independent reader. `vfx.module.save`/`vfx.module.read` persist it with undo/redo through the shared command registry. System drafts keep module names rather than copying nodes. The project asset layer still needs to resolve those names to explicit asset references and send their sources with compile/preview requests; the engine must validate stage and interface compatibility, reject missing assets and cycles with named diagnostics, compose the graphs, and include each dependency's semantic digest in the cook key. `read_authoring_document` currently rejects every nonempty module list, so module references remain unavailable until this entire path is implemented (tracked by #19). The service must not infer a module file from an unqualified name or silently omit one.

## Preview and inspection

An isolated engine preview instance drives play, pause, restart, scrub, and time scale. The viewport displays the runtime result. Debug snapshots expose bounded per-emitter counts, budget state, event traffic, and one-particle attribute readback. Unsupported renderer kinds or target capabilities return named refusals.

## Vertex material stage

The existing material graph gains an explicit vertex stage and typed outputs for world-position offset, custom interpolants, and displacement. Stage-aware nodes use the same canvas and backend catalogue. Compilation reports variants by geometry source. Unsupported paths, including virtual geometry where offset evaluation is unavailable, fail in both editor validation and cook. The same vertex expression drives the main, shadow, and previous-frame positions so motion vectors follow displacement.

## Verification

Use compiler-registry parity, service, save/reopen/cook, transaction/MCP, and live preview tests. Image tests compare VFX and displaced material output to committed references; displaced shadows and motion vectors are compared with CPU-displaced geometry. For each acceptance criterion, record a mutation that makes its ledger criterion fail.
