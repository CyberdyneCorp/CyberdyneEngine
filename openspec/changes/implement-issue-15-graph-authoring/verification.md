# Verification ledger

This ledger records executable evidence for issue #15. It is incomplete until every acceptance criterion has a green check and a recorded red mutation.

## VFX palette and compiler registry

- **Open:** `vfx_editor_opens_on_the_shared_canvas_from_backend_nodes` opens `Domain::VfxGraph` using a backend-supplied catalogue and edits it on the shared canvas.
- **Parity:** `editor_backend: VFX palette equals the compiler registry` compares every serialized node identity, name, pin, and property with `register_vfx_nodes`, including generated data-interface field nodes.
- **Green command:** `build/dev/cy_test_unit_editor_backend --test-case='editor_backend: VFX palette equals the compiler registry' --no-skip` — 1 case, 2,564 assertions passed on macOS.
- **Red mutation:** in `src/vfx/src/catalogue.cpp`, temporarily serialize `registry.size() - 1` and skip the final registered node. The same command failed with the reported count `80` against the registry's `81` and missing-node assertions. Restoring the serializer made all 2,564 assertions pass. The mutation is not committed.

## VFX compiler diagnostic location

- **Service:** `editor_backend: VFX compiler diagnostics name the authored node` submits an invalid Spawn-stage graph to `vfx.compile` and checks the engine diagnostic, node ID, emitter index, and stage in the editor service response. The panel uses that scope to select and outline the offending node.
- **Green command:** `build/dev/cy_test_unit_editor_backend --test-case='editor_backend: VFX compiler diagnostics name the authored node' --no-skip` — 1 case, 25 assertions passed on macOS.
- **Red mutation:** in `src/editor_backend/src/material_service.cpp`, temporarily encode node ID `0` instead of `diagnostic.node`. The same command failed at the expected node ID (`0` versus `1`). Restoring the encoded compiler node ID made all 25 assertions pass. The mutation is not committed.

## Live VFX parameter versus graph compilation

- **Signature:** `compile_signature_ignores_layout_and_live_values_but_tracks_graph_edits` checks that an exposed parameter value and canvas layout leave the compile signature unchanged, while a graph node change and folded parameter value change alter it. `graph_edits_submit_a_new_cook_but_live_values_do_not` checks the resulting submission behavior in the desktop panel.
- **Green command:** `cargo test --manifest-path editor/Cargo.toml -p cy-editor-interface compile_signature_ignores_layout_and_live_values_but_tracks_graph_edits --quiet` — 1 targeted test passed on macOS.
- **Red mutation:** in `editor/crates/cy-editor-interface/src/specialised/vfx.rs`, temporarily leave exposed parameter values in `compile_signature`. The test failed when changing `speed` from `2` to `4` altered the signature. Restoring the normalization made it pass. The mutation is not committed.

## VFX stage editing through MCP

- **Wire path:** `vfx_stage_wire_and_property_round_trip_over_mcp` adds stage nodes, edits a property, connects and moves nodes, disconnects and removes them, and checks the saved document and history through MCP. Other wire cases cover emitter, parameter, module, and preview commands.
- **Green command:** `cargo test --manifest-path editor/Cargo.toml -p cy-editor-mcp --test a_session_over_the_wire vfx_stage_wire_and_property_round_trip_over_mcp --quiet` — 1 targeted test passed on macOS.
- **Red mutation:** temporarily omit `connect_nodes()` from `vfx_authoring_commands::register`. The same wire test failed when `vfx.node.connect` returned an MCP error. Restoring registration made the test pass. The mutation is not committed.
- **Remaining work:** task 2.6 still tracks individual typed desktop transactions for unsaved canvas gestures and a complete desktop/MCP parity check.

## Vertex material save and history through MCP

- **Wire path:** `vertex_material_canvas_saves_and_undoes_over_mcp` assigns the material to a scene mesh, saves a vertex graph through `material.graph.save`, and checks that the request carries `StaticMesh`. It receives the engine-authored canonical graph, reads the editable canvas through MCP, then checks that one undo removes both project files and redo restores both.
- **Green command:** `cargo test --manifest-path editor/Cargo.toml -p cy-editor-mcp --test a_session_over_the_wire vertex_material_canvas_saves_and_undoes_over_mcp --quiet` — 1 targeted test passed on macOS.
- **Red mutation:** in `editor/crates/cy-editor-services/src/editor.rs`, temporarily omit the material save transaction's `document.commit()`. The wire test failed when `edit.undo` returned an MCP error. Restoring the commit made it pass. The mutation is not committed.
- **Regression red:** with the scene mesh assigned, the material save created a second `Sync material properties` history entry after its file transaction. The strengthened wire test failed because one undo left both graph files in place. Moving property sync inside the save transaction made one undo restore the files and fields together.
- **Remaining work:** material canvas gestures still need individual transaction/MCP parity, tracked by task 3.4.

## Assigned geometry at material authoring

- **Service:** `editor_backend: material authoring refuses unsupported assigned geometry` sends a vertex graph with a `VirtualGeometry` assignment to `material.author` and checks the compiler's named `vertex-geometry-unsupported` refusal. The same case checks that a `StaticMesh` assignment is authored.
- **Green command:** `build/dev/cy_test_unit_editor_backend --test-case='editor_backend: material authoring refuses unsupported assigned geometry' --no-skip` — 1 case, 12 assertions passed on macOS.
- **Regression red:** before the author path called `compile_material` for assigned geometry, the new test received a completed author response instead of the refusal. The check now runs before the canonical graph is returned.
- **Scope:** scene discovery currently supplies static meshes. Other geometry source assignments remain part of task 3.2.

## Vertex geometry variants and unsupported paths

- **Compiler:** `material_lowering: named geometry variants reject unsupported vertex paths` compiles a vertex graph for static and skinned meshes, checks the two reported variants, and verifies the `vertex-geometry-unsupported` diagnostic for virtual geometry. It also checks an unknown geometry source.
- **Editor service:** `editor_backend: assigned geometry paths use the material compiler's refusal` submits the source names through the editor service and checks that the unsupported path returns the compiler diagnostic.
- **Green command:** `ctest --test-dir build/dev -R '^(integration\.material_lowering|unit\.editor_backend)$' --output-on-failure` — both suites passed on macOS. The targeted compiler case ran with 15/15 assertions.
- **Red mutation:** in `src/rendering/material/src/compiler.cpp`, temporarily allow `VirtualGeometry` through `check_geometry_paths`. The targeted compiler case failed its refusal, diagnostic, and named-path assertions (12/15 passed). Restoring the refusal made all 15 assertions pass. The mutation is not committed.
- **Remaining work:** the material author/save path currently bypasses `compile_material` and does not carry assigned geometry sources. Task 3.2 stays open until that path rejects unsupported vertex geometry. A rendered displaced mesh, shadow, and motion-vector comparison against CPU-displaced geometry is tracked by task 3.3.

## Two-emitter sample image

- **Source:** `samples/05b-editor-window/project/effects/issue15_two_emitters.cyvfxdoc`.
- **Save and reopen:** `committed_two_emitter_sample_reopens_without_losing_stage_graphs` decodes and re-encodes this exact draft, checking both simulation paths and stage snapshots. The editor's fresh-window `vfx_draft_saved_through_command_reopens_in_a_fresh_window` test covers the project save/read command path.
- **Cook and preview:** `unit.editor_backend` compiles that exact draft with CPU and GPU emitters; `integration.editor_backend_compile` loads it into the engine preview and inspects live particles.
- **Rendered comparison:** `smoke.editor_authored_frame_metal` advances the engine preview, checks particle publication, and compares its 640×360 frame against `samples/05b-editor-window/runtime/tests/references/issue15_two_emitters_metal.png` using the shared golden-image metric. It also verifies that removing the preview restores the baseline frame.
- **Green command:** `ctest --test-dir build/dev -R '^(unit\.editor_backend|integration\.editor_backend_compile|smoke\.editor_authored_frame_metal)$' --output-on-failure` — 3/3 passed on macOS Metal.
- **Red mutation:** immediately after `render_test::adopt` in the Metal case, temporarily apply `captured.texels[0] ^= 0x00ffffffU`. The test fails with one differing texel away from edges and worst channel delta 255 at `(0, 0)`. Remove the mutation, rebuild, and rerun; 3/3 pass. The mutation is not committed.
- **Scope:** the Metal comparison is verified on this Mac. The Vulkan two-emitter image case is present but its pixel assertion has not run here because no Vulkan device is available.
