# Design

## Shared graph authoring

Use `cy_editor_interface::specialised::GraphCanvas` for both domains. The engine service publishes versioned catalogues, including stable identities, typed pins, properties, capabilities, and stage metadata. The editor stores node instances and layout separately; graph validation and compilation remain in the engine. A catalogue addition must reach the palette without editing a Rust node list.

## VFX asset and compilation path

The authored hierarchy is system → emitters → spawn/update/event/render stage graphs. Modules are separately saved graph assets referenced by stages. Each emitter declares its CPU or GPU target, renderer, user parameters, and data-interface bindings. Editor commands produce document transactions, and the same commands back MCP. Saved-document commands now cover emitter removal, capacity, particle attribute declarations, and event channel bounds with undo/redo; wire tests check valid edits and atomic refusal of unknown emitters and invalid capacity. The panel preserves other unsaved stages when an emitter is removed. The service compiles through `cy::vfx-compiler`, returning `CompileReport`, attribute layout, generated source on demand, and node-located diagnostics. Save writes a reopenable authoring document and canonical cook input. Runtime parameter updates use the live instance path; graph edits schedule compilation.

The editable draft is a versioned `.cyvfxdoc` project source. `vfx.document.save` journals its prior and new contents in the active scene document so undo/redo restores the file; `vfx.document.read` reopens it through the same command registry. The engine service will produce canonical cook input from this document after stage validation and compilation are connected. The draft format is not itself a cooked effect.

Reusable modules have a separate versioned `.cyvfxmodule` source contract: compatible stage, typed host inputs, dependency names, and shared-canvas graph. The editor can encode and reopen the asset, and the engine has an independent reader. `vfx.module.save`/`vfx.module.read` persist it with undo/redo through the shared command registry. System drafts keep module names rather than copying nodes. Version 3 of the binary `.cyvfxdoc` payload maps each name to an explicit project-relative `.cyvfxmodule` path; version 1 and 2 drafts still reopen. The project layer now loads the mapped sources for compile and preview, and the engine validates typed inputs, stage compatibility, missing assets, and dependency cycles before composing their graphs into emitter stages. Composed graph digests contribute to the cook key. The panel creates and opens modules on the shared canvas, edits their stage, typed host inputs and dependencies, and saves them through `vfx.module.save`. Attaching a saved module to an emitter records its explicit project path through `vfx.document.save`. Both saves participate in scene undo/redo. Opening or creating another module refuses to replace unsaved edits; the panel explicitly offers discard to reload the saved source or close an unsaved new module. Automatic compile signatures include saved module content while ignoring canvas layout. Saved module node insertion, connection, disconnection, removal, and property edits now use engine-catalogue commands shared with MCP and save through project undo history. The panel's unsaved canvas edits still need individual transactions, and full MCP parity remains task 2.6. The service must not infer a module file from an unqualified name or silently omit one.

## Preview and inspection

An isolated engine preview instance drives play, pause, restart, scrub, and time scale. The viewport displays the runtime result. Debug snapshots expose bounded per-emitter counts, budget state, event traffic, and one-particle attribute readback. Unsupported renderer kinds or target capabilities return named refusals.

## Vertex material stage

The existing material graph gains an explicit vertex stage and typed outputs for world-position offset, custom interpolants, and displacement. Stage-aware nodes use the same canvas and backend catalogue. Compilation reports variants by geometry source. Unsupported paths, including virtual geometry where offset evaluation is unavailable, fail in both editor validation and cook. The same vertex expression drives the main, shadow, and previous-frame positions so motion vectors follow displacement.

The material IR first carries a typed `float3` world-space vertex offset root beside its surface
and opacity roots. Graph and text authoring lower to that same root; its content enters the module
digest and versioned encoding. Optimisation and shadow derivation retain it so later vertex shader
emission reads one expression across visible, shadow, and previous-frame positions. Each compiled
variant carries a separate generated vertex function from this root, using the same SSA expression
writer as the surface function. The vertex function is compiled against the engine Slang library as
a vertex-stage probe before frame integration.
Cooked material bundle version 2 carries each variant's vertex source and digest beside its
fragment source. Opaque shadow variants retain the vertex source despite having no fragment work;
the reader continues to accept older version 1 bundles, and the compiler version forces recooking.
The hosted Metal material program now places that generated vertex function in its shader unit and
adds its world-space result before projection. Visible and shadow vertex entry points call the same
displaced-position helper and bind the same material parameter block. A shader-unit regression
checks both calls; the Metal image case compares the constant GPU offset, including its shadow,
against moving the same mesh on the CPU. Motion vectors and a time-varying CPU reference remain
before task 3.3 is complete.

The sine sway example first needs numeric sine in the material vocabulary. `Sin` is appended to the material IR and graph operation enums, preserving existing operation identities. The text front end and engine-owned node palette both lower it to the same typed IR operation; the emitter writes Slang `sin` and constant folding uses the same radian operation. This arithmetic addition is shared by surface and future vertex expressions and does not itself enable vertex outputs.

Material catalogue schema 3 publishes a stage mask per node from the engine vocabulary: surface only for closures, texture samples, custom Slang, and the surface output; shared for typed numeric inputs and math. The editor reads older schemas with an unrestricted mask and filters its surface palette using the engine value. Vertex-only nodes and the vertex canvas will use the same mask rather than a second Rust vocabulary.

## Verification

Use compiler-registry parity, service, save/reopen/cook, transaction/MCP, and live preview tests. Image tests compare VFX and displaced material output to committed references; displaced shadows and motion vectors are compared with CPU-displaced geometry. For each acceptance criterion, record a mutation that makes its ledger criterion fail.
