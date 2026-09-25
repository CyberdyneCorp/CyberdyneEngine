# Design

## Context

The editor already owns external FBX staging, importer invocation, world transactions, camera input, and Move, Rotate, and Scale gizmos. Its host currently converts each world node to a fixed first-light box. `FrameAssembly` and `FrameRecorder` provide the engine frame, but the Metal frame shaders were compiled as independent MSL buffers while the Metal RHI binds argument buffers by descriptor set.

## Goals / Non-Goals

**Goals:** Make the viewport draw the mesh identities and transforms recorded in the authored world, on Metal and Vulkan, and make the Metal capture test enforce visible output.

**Non-Goals:** Replace the editor's transaction protocol, change `.cyworld`, replace the external importer, or implement mesh skinning and animation in this change.

## Decisions

1. **Use one frame path.** The host builds a spatial index and instance transform table from `ser::World`, then drives `FrameAssembly`, `FrameRecorder`, and `GraphExecutor`. This meets the viewport's same-renderer contract and avoids extending the first-light sample renderer.
2. **Resolve meshes from existing import output.** A `.cyprim` reference is generated through `cy::import`; an imported mesh identity reads `.cy/cooked/<id>.cyasset` and `read_cooked_mesh`. The host caches GPU streams by mesh reference and invalidates them when imports change. No FBX parser enters the runtime.
3. **Preserve hierarchy and transforms.** Compose each node's local transform with its parent before writing camera-relative instance rows. Compute world bounds from mesh bounds and the full transform; use those bounds for picking and framing.
4. **Match the Metal ABI.** Generate MSL with a frame view `ParameterBlock`; remap Slang's compacted Metal buffer indices to the RHI's fixed set and push-constant indices during embedding. The Metal render test asserts pixels and runs in mixed backend builds.
5. **Keep the current transport.** The rendered output is read back to the publisher's staging image; selection marker and gizmo geometry remain tied to the published view and stable identity.
6. **Carry FBX base-colour textures through cooking.** Use ufbx's embedded texture bytes when available and the import resolver for an external texture. Produce a cooked texture sub-asset with a stable name, then resolve that name to its minted `AssetId` after sub-asset binding. Store the resolved texture identity in the cooked material, preserving the existing material reader's ability to load older untextured records. The editor loads the cooked texture and binds its material slot through the frame's texture table.
7. **Sample on native backends.** The Vulkan frame already declares material texture slots. Replace the Metal frame's white sampling stub with a bounded argument-buffer texture table at set 0, and regenerate the committed shader artefacts. A rendered texture-vs-constant comparison on Metal must differ on mesh pixels.
8. **Preserve texture layout from source to GPU.** Flip FBX image V coordinates into the renderer's top-origin convention during import. Compute Metal buffer-to-texture and texture-to-buffer row strides in compressed blocks for BC formats. Compare the same FBX in Blender and the authored Metal frame to verify continuous texture placement on the mesh.
9. **Synchronize new world declarations before live edits.** Transactions name component and field numbers declared in the world file. An empty world has no declarations, so a newly imported FBX can create a node in the runtime without giving the renderer enough schema to find its mesh. On attachment with unsaved content and whenever the editor's document schema changes, send the current unsaved `.cyworld` text through one bounded `SyncWorld` message. The runtime parses it into a temporary world, checks document identity, resolves engine types, and replaces its in-memory world before subsequent transactions. Ordinary edits continue to use the journal's `Apply` stream, including replay after a runtime restart; an unsaved schema change uses a snapshot on reconnect.
10. **Preview open transactions in the engine.** A gizmo writes intermediate transforms into the document while its transaction is open, but the mirror's committed-history stream cannot see them. During an open transaction, send the current world snapshot when its content revision changes, at most once per editor frame. On commit or cancel, send the final document snapshot and align the mirror's history cursor so the committed drag is not applied twice. Ordinary committed edits continue through `Apply`.
11. **Keep viewport chrome compact.** Draw the existing square product mark in the menu header. Give each numeric transform row a fixed label column and fixed-width fields. Give the orientation overlay a lighter translucent fill while retaining contrast for its axis labels and hit targets.

## Risks / Trade-offs

- **Mesh or material missing** → Report the reference and render a clear diagnostic; do not silently draw a box.
- **Large imported FBX** → Upload each distinct mesh once and share buffers between instances; report an explicit capacity failure.
- **Metal shader compiler changes its generated slots** → The embedding step checks the expected input signature and fails during regeneration when it changes.
- **Large worlds during a live drag** → Send no more than one snapshot for a changed content revision per editor frame; retain the mesh cache in the runtime so moving an instance does not recook or reupload geometry.
