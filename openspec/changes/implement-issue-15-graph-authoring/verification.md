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
- **Parity check:** saved desktop canvas gestures and metadata actions use the same registered `vfx.*` commands exposed through MCP. New unsaved drafts edit their local canvas until saved. `cargo test --manifest-path editor/Cargo.toml -p cy-editor-mcp --test a_session_over_the_wire vfx_ --quiet` passed 11 wire tests, including history, stage graphs, emitters, modules, declarations, and refusals; `cargo test --manifest-path editor/Cargo.toml -p cy-editor-shell --lib saved_vfx --quiet` passed 6 desktop routing tests; and the `saved_emitter` shell filter passed 3 tests on 2026-09-27.

## Desktop VFX palette command parity

- **Desktop path:** `saved_palette_additions_use_the_same_commands_as_mcp` checks that adding a node to a saved system stage or module queues `vfx.node.add` or `vfx.module.node.add` with the open asset reference, stage, type, and coordinates, without mutating the canvas directly. A new draft still receives a local node.
- **Green command:** `cargo test --manifest-path editor/Cargo.toml -p cy-editor-shell --lib saved_palette_additions_use_the_same_commands_as_mcp` — 1 targeted test passed on macOS.
- **Red mutation:** temporarily make `add_palette_node` always call `canvas.add`. The test failed because the saved canvas gained a node instead of queuing the typed command. Restoring command routing made it pass. The mutation is not committed.
- **Movement:** saved node movement queues one typed command at drag release, with history covered below.

## Desktop VFX property command parity

- **Desktop path:** `saved_vfx_property_edits_use_the_same_commands_as_mcp` checks that editing a property on a saved system stage or module queues `vfx.node.property.set` or `vfx.module.node.property.set` with the selected node, engine property name, and new value. The saved canvas stays unchanged until the command executes; a new draft updates locally.
- **Green command:** `cargo test --manifest-path editor/Cargo.toml -p cy-editor-shell --lib saved_vfx_property_edits_use_the_same_commands_as_mcp` — 1 targeted test passed on macOS.
- **Red mutation:** temporarily make `edit_node_property` write directly to a saved canvas. The test failed because the local property changed from `1` to `3` before a command was queued. Restoring command routing made it pass. The mutation is not committed.

## Desktop VFX connection command parity

- **Desktop path:** `saved_vfx_pin_connections_use_the_same_commands_as_mcp` checks saved system and module connections queue `vfx.node.connect` or `vfx.module.node.connect` with the node keys and engine pin names. `pin_action_can_route_a_connection_without_mutating_the_canvas` checks that the shared pin gesture invokes this hook. A new draft connects locally, while an invalid pin identity is refused before a command is queued.
- **Green command:** `cargo test --manifest-path editor/Cargo.toml -p cy-editor-shell --lib saved_vfx_pin_connections_use_the_same_commands_as_mcp` — 1 targeted test passed on macOS.
- **Red mutation:** temporarily make `connect_nodes` write directly to the saved canvas. The test failed because a link appeared locally instead of a typed command being queued. Restoring command routing made it pass. The mutation is not committed.

## Desktop VFX selected-node removal

- **Desktop path:** `selected_vfx_node_removal_uses_the_same_commands_as_mcp` checks that the panel action queues `vfx.node.remove` or `vfx.module.node.remove` with the saved reference and selected key, while a new draft removes locally. It checks an unknown node is refused without queuing a command.
- **Green command:** `cargo test --manifest-path editor/Cargo.toml -p cy-editor-shell --lib selected_vfx_node_removal_uses_the_same_commands_as_mcp` — 1 targeted test passed on macOS.
- **Red mutation:** temporarily make `remove_selected_node` remove directly from a saved canvas. The test failed because the saved canvas lost its node before any command ran. Restoring command routing made it pass. The mutation is not committed.

## Desktop VFX selected-wire disconnection

- **Desktop path:** `selected_vfx_wire_disconnection_uses_the_same_commands_as_mcp` checks that a selected node's wire action queues `vfx.node.disconnect` or `vfx.module.node.disconnect` with the exact saved wire endpoints. A new draft disconnects locally, and an absent wire is refused without queuing a command.
- **Green command:** `cargo test --manifest-path editor/Cargo.toml -p cy-editor-shell --lib selected_vfx_wire_disconnection_uses_the_same_commands_as_mcp` — 1 targeted test passed on macOS.
- **Red mutation:** temporarily make `disconnect_link` remove a wire directly from the saved canvas. The test failed because the saved canvas lost its wire before any command ran. Restoring command routing made it pass. The mutation is not committed.

## Desktop VFX node drag command parity

- **Desktop path:** `saved_vfx_drag_moves_locally_then_queues_one_typed_command` checks that intermediate drag positions update the displayed canvas without an intent, and release queues one `vfx.node.move` or `vfx.module.node.move` with the saved asset identity, node, and final position. A draft without a saved path moves locally.
- **Journal path:** `typed_vfx_drag_release_skips_whole_document_save` checks that release retains only the typed move intent and does not prepend a whole-document save.
- **Green command:** `cargo test --manifest-path editor/Cargo.toml -p cy-editor-shell --lib saved_vfx_drag_moves_locally_then_queues_one_typed_command` and `cargo test --manifest-path editor/Cargo.toml -p cy-editor-shell --lib typed_vfx_drag_release_skips_whole_document_save` — each targeted test passed on macOS.
- **Red mutations:** temporarily omit the release `intents.push`; the first test failed because no move intent was queued. Temporarily remove the journal's typed move suppression; the second test failed because release produced two intents instead of one. Both mutations were restored.

## Desktop VFX emitter creation command parity

- **Desktop path:** `saved_emitter_creation_uses_the_mcp_command_and_new_drafts_select_spawn` checks that a saved system queues `vfx.emitter.add` with the open reference, name, CPU/GPU target, and renderer without mutating the draft. A new system adds locally and opens its Spawn stage.
- **History path:** `saved_emitter_addition_selects_spawn_and_undoes_with_one_command` invokes the same command in a saved desktop window, checks that the new Spawn stage is selected, and checks one undo and redo restore the project source.
- **Green command:** `cargo test --manifest-path editor/Cargo.toml -p cy-editor-shell --lib saved_emitter --quiet` — the focused shell tests passed on macOS.
- **Red mutations:** temporarily force saved additions down the draft path; the panel test failed when the saved draft gained an emitter. Temporarily disable the post-command stage selection; the window test failed because it stayed on emitter zero. Both mutations were restored.

## Vertex material save and history through MCP

- **Wire path:** `vertex_material_canvas_saves_and_undoes_over_mcp` assigns the material to a scene mesh, saves a vertex graph through `material.graph.save`, and checks that the request carries `StaticMesh`. It receives the engine-authored canonical graph, reads the editable canvas through MCP, then checks that one undo removes both project files and redo restores both.
- **Green command:** `cargo test --manifest-path editor/Cargo.toml -p cy-editor-mcp --test a_session_over_the_wire vertex_material_canvas_saves_and_undoes_over_mcp --quiet` — 1 targeted test passed on macOS.
- **Red mutation:** in `editor/crates/cy-editor-services/src/editor.rs`, temporarily omit the material save transaction's `document.commit()`. The wire test failed when `edit.undo` returned an MCP error. Restoring the commit made it pass. The mutation is not committed.
- **Regression red:** with the scene mesh assigned, the material save created a second `Sync material properties` history entry after its file transaction. The strengthened wire test failed because one undo left both graph files in place. Moving property sync inside the save transaction made one undo restore the files and fields together.
- **Remaining work:** material canvas gestures still need individual transaction/MCP parity, tracked by task 3.4.

## Vertex material preview on mesh and scene

- **Wire path:** `vertex_material_canvas_previews_over_mcp_without_saving` sends the editable vertex canvas through `material.graph.preview`, observes the hosted `material.preview.set` request and completed response, and verifies that preview creates no project graph asset. The desktop panel submits the same canvas through `Editor::preview_material_graph` when its semantic graph content changes.
- **Scene compilation:** `authored scene compiles a surface beside its vertex graph` compiles the authored vertex expression for visible, depth, and shadow passes. The hosted mesh pixel case `a compiled vertex offset moves the hosted Metal material mesh` exists, but its pixel assertions require a native Metal device.
- **Green commands:** the focused MCP test passed (1/1), and the authored scene compilation case passed (26/26 assertions) on 2026-09-27. The local hosted mesh case selected the null RHI and executed only its availability assertion. Task 3.4 remains open for a native preview image and individual material canvas transaction parity.
- **Preview refusal regression:** `authored scene preview refuses an unbound vertex environment field` first accepts a valid graph, then submits a typed `wind` field to the vertex output. The engine now refuses it before changing the preview and renders the previously accepted material. The focused null-RHI case passed 13/13 assertions on 2026-09-27.
- **Red mutation:** before the preview preflight called `compile_scene_graph_material` and `assemble_scene_material_vertex_unit`, the focused case failed because `preview()` accepted the unbound field. The subsequent read of the nonexistent error also tripped the test harness. With preflight restored, it passes.
- **Complexity:** the cognitive-complexity skill measured `AuthoredFrame::preview` at 9, within the systems target.

## Assigned geometry at material authoring

- **Service:** `editor_backend: material authoring refuses unsupported assigned geometry` sends a vertex graph with a `VirtualGeometry` assignment to `material.author` and checks the compiler's named `vertex-geometry-unsupported` refusal. The same case checks that a `StaticMesh` assignment is authored.
- **Green command:** `build/dev/cy_test_unit_editor_backend --test-case='editor_backend: material authoring refuses unsupported assigned geometry' --no-skip` — 1 case, 12 assertions passed on macOS.
- **Regression red:** before the author path called `compile_material` for assigned geometry, the new test received a completed author response instead of the refusal. The check now runs before the canonical graph is returned.
- **Scope:** desktop and MCP save now pass assigned static-mesh geometry to authoring. Other geometry source assignments remain part of task 3.2.

## Vertex geometry variants and unsupported paths

- **Compiler:** `material_lowering: named geometry variants reject unsupported vertex paths` compiles a vertex graph for static and skinned meshes, checks the two reported variants, and verifies the `vertex-geometry-unsupported` diagnostic for virtual geometry. It also checks an unknown geometry source.
- **Editor service:** `editor_backend: assigned geometry paths use the material compiler's refusal` submits the source names through the editor service and checks that the unsupported path returns the compiler diagnostic.
- **Green command:** `ctest --test-dir build/dev -R '^(integration\.material_lowering|unit\.editor_backend)$' --output-on-failure` — both suites passed on macOS. The targeted compiler case ran with 15/15 assertions.
- **Red mutation:** in `src/rendering/material/src/compiler.cpp`, temporarily allow `VirtualGeometry` through `check_geometry_paths`. The targeted compiler case failed its refusal, diagnostic, and named-path assertions (12/15 passed). Restoring the refusal made all 15 assertions pass. The mutation is not committed.
- **Remaining work:** author/save now checks geometry sources supplied by the editor, but scene discovery only identifies static-mesh assignments. Task 3.2 stays open for other geometry sources. A rendered displaced mesh, shadow, and motion-vector comparison against CPU-displaced geometry is tracked by task 3.3.

## Authored scene vertex animation across frames

- **Scene path:** the authored frame accepts a caller-supplied time for deterministic material animation. `authored scene compiles a surface beside its vertex graph` now compiles a sine/time vertex graph for visible, depth, and shadow stages and checks that previous-frame evaluation subtracts the frame delta. A two-frame native test compares both the temporal image and packed prepass motion with the same mesh translated on the CPU by `4 * sin(0.05)`; motion readback is opt-in.
- **Green commands:** `build/dev/cy_test_smoke_editor_authored_frame_metal --test-case='authored scene compiles a surface beside its vertex graph,authored frame refuses nonfinite material animation time' --no-skip` — shader compilation, source assertions, finite-time refusal, and a null-RHI command-log check of the opt-in motion copy passed on macOS.
- **Red mutations:** temporarily evaluate the previous position at current time in `material_runtime.cpp`; the compile test failed its previous-time assertion (25/26 passed). Temporarily omit the motion copy callback; the null-RHI test failed because only one texture copy occurred instead of two (7/8 passed). Both mutations were restored.
- **Native pixel status:** the local RHI selected its null fallback, so the two-frame image and motion comparisons did not execute here. Task 3.3 remains open pending a native Metal or Vulkan run.
- **Complexity:** the cognitive-complexity skill measured the changed authored-frame functions after extracting motion capture: `capture` 32, `initialize` 25, `render` 15, `record_motion_capture` 1, and `copy_motion_readback` 4. All are within the systems target of 25–35 for inherently branchy frame setup.

## Two-emitter sample image

- **Source:** `samples/05b-editor-window/project/effects/issue15_two_emitters.cyvfxdoc`.
- **Save and reopen:** `committed_two_emitter_sample_reopens_without_losing_stage_graphs` decodes and re-encodes this exact draft, checking both simulation paths and stage snapshots. The editor's fresh-window `vfx_draft_saved_through_command_reopens_in_a_fresh_window` test covers the project save/read command path.
- **Cook and preview:** `unit.editor_backend` compiles that exact draft with CPU and GPU emitters; `integration.editor_backend_compile` loads it into the engine preview and inspects live particles.
- **Rendered comparison:** `smoke.editor_authored_frame_metal` advances the engine preview, checks particle publication, and compares its 640×360 frame against `samples/05b-editor-window/runtime/tests/references/issue15_two_emitters_metal.png` using the shared golden-image metric. It also verifies that removing the preview restores the baseline frame.
- **Green command:** `ctest --test-dir build/dev -R '^(unit\.editor_backend|integration\.editor_backend_compile|smoke\.editor_authored_frame_metal)$' --output-on-failure` — 3/3 passed on macOS Metal.
- **Red mutation:** immediately after `render_test::adopt` in the Metal case, temporarily apply `captured.texels[0] ^= 0x00ffffffU`. The test fails with one differing texel away from edges and worst channel delta 255 at `(0, 0)`. Remove the mutation, rebuild, and rerun; 3/3 pass. The mutation is not committed.
- **Scope:** the Metal comparison is verified on this Mac. The Vulkan two-emitter image case is present but its pixel assertion has not run here because no Vulkan device is available.
