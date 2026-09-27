# Verification ledger

This ledger records executable evidence for issue #15. It is incomplete until every acceptance criterion has a green check and a recorded red mutation.

Run `python3 tools/issue15_acceptance.py --list` to inspect the nine issue criteria or
`python3 tools/issue15_acceptance.py` to execute their current probes. The runner exits nonzero
while any criterion is unverified. Native image probes require an executed test and enough
assertions to exclude the null-device availability branch; a successful doctest exit alone does
not count as pixel evidence. `python3 tools/test_issue15_acceptance.py` checks this guard. On this
Mac, the full ledger verified four of nine criteria: both native image probes ran only two
availability assertions, and native images of the two-emitter sample and displaced material,
along with virtual-geometry scene discovery, are open. The final
regression/documentation audit is also open. These gaps stay visible in the
runner rather than being inferred from neighbouring green tests.
The runner's four unit tests passed. It also rejects a successful Cargo invocation whose filter
selected no tests; a matching unit test proves that guard. The palette, recook, and diagnostic
criteria verified through the runner on 2026-09-27. The history criterion also verifies: desktop
system creation, system and module gesture history, VFX system and module MCP edits, and desktop,
MCP, and draft material history. The desktop's New VFX actions first create saved assets through
registered commands, so ordinary user gestures have an undoable project path immediately. The
remaining five criteria stay open.
Removing the Cargo selection guard made `test_cargo_filter_must_execute_a_test` fail because an
empty filter was reported as passed; restoring the guard returned all four runner tests to green.
Temporarily bypassing its native assertion-count check
made two unit tests fail, then restoration passed; this guards against a false green on null
devices. The cognitive-complexity skill could not score these Python functions locally because
`complexipy` is not installed; syntax parsing and focused tests passed.

The Engine frame now carries `cy.field`'s storage-buffer table at set 0 binding 3. The null-device
`the frame writes Engine field images at cy.field's global binding` regression accepts a live
buffer, refuses duplicate, missing and out-of-range entries, then destroys the buffer and confirms
the next descriptor update fails. That last control would pass incorrectly if binding 3 were not
written. The focused case passed 10 assertions and the full `integration.render_pipeline` suite
passed 14 cases and 174 assertions on macOS. Native CPU/GPU value and pixel comparison remain open.
Removing the binding-3 write loop made that focused regression fail (8/10 assertions passed);
restoring the loop returned it to green. The mutation is not committed.

An MCP attempt to capture the new sine-sway scene could not provide native image evidence on this
host: the editor-window runtime logged `MTLCreateSystemDefaultDevice returned no device`, selected
the null backend, and did not open its viewport sockets. The editor-only `editor:window` request
also supplied no reply. That exposed a separate capture-client bug: `Mcp.call` used a blocking
`readline()` inside a nominal timeout. It now reads available pipe bytes up to a deadline, retaining
partial JSON across reads. `test_mcp_window.py` passed three no-reply, partial-reply, and buffered-
reply cases. Replacing the bounded read with `readline()` made the test process exceed its one-
second mutation timeout; restoration passed all three cases. A real editor-only request returned
`resources/read had no answer within 3 s` instead of hanging. The quality CI job runs these
dependency-free tests. No new screenshot or
native displacement proof is claimed from this attempt.

The quality job initially refused the direct Python test command under `just ci-check`:
workflow commands must call recipes. `just quality-editor-mcp-client` now runs the same three tests,
and the workflow calls that recipe. A `check_workflows.py --selftest` fixture rejects the original
direct command and accepts the recipe, so this CI regression is covered by a negative case.

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

- **System creation:** `vfx_system_is_created_with_two_emitters_and_reopened_over_mcp` starts from an empty project, calls `vfx.document.create`, adds one CPU and one GPU emitter, and adds, edits, and connects Spawn nodes on both through the engine catalogue response. It reads the saved document, refuses an overwrite, then undoes all eleven transactions through deletion and redoes the same source. A red mutation omitting the create command registration made the first MCP call fail; restoration passed.
- **Exact sample authoring:** `committed_two_emitter_sample_can_be_authored_through_mcp_commands` builds the committed sample's declarations, bindings, and both Spawn and Initialise graphs using MCP commands, then compares every saved graph fact except layout with the committed file and reopens it over MCP. The test batches calls within the MCP invocation budget.
- **Vector literal regression:** The engine catalogue now describes `vfx.constant` as a one-to-four-component text literal, matching its canvas reader. The editor validates its finite space-separated components. The MCP sample test uses catalogue schema 2 and refuses a five-component literal without changing the saved asset. The editor validator test first failed on an empty literal, and the engine catalogue test first failed on the old scalar descriptor. Removing the schema-2 `vfx-literal` semantic made the MCP refusal fail; restoration passed.

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
- **Draft gesture history:** `material.canvas.draft.save` records editable source before a canonical `.cygraph` exists. The desktop routes new-canvas add, connect, move, property, disconnect, and remove gestures through this command; MCP uses the same source-only transaction for its material node commands. `material_draft_gestures_undo_before_canonical_authoring` verifies save, add, undo, and redo over MCP with no canonical file, and `unsaved_material_canvas_undoes_before_engine_authoring` checks the open desktop canvas and sidecar. A red mutation omitting draft command registration failed the MCP test at its first save; restoration passed. The 102 shell, 167 service, and 31 MCP wire tests, plus all-target Clippy for those crates, passed on 2026-09-27. Task 3.4 remains open for preview-mesh evidence and the full acceptance ledger.
- **Desktop parity regression:** the desktop Save button now queues `material.graph.save`, the same registered command as MCP. `desktop_material_save_uses_the_mcp_command_and_undo_path` checks the queued reference and editable canvas. `desktop_material_save_and_mcp_share_one_undoable_transaction` drives the command through the desktop window, answers the engine author request, checks both files and the success notification, then undoes and redoes the graph. The focused shell tests passed 2/2 and the MCP save test passed 1/1 on 2026-09-27.
- **Red mutation:** before routing the desktop action through the registered command, its focused test failed because no command intent was queued. The old desktop writer persisted both files outside the project history; the command path now owns the save and property sync transaction. All 99 shell unit tests and all-target shell/services Clippy passed after the change.

## Vertex material preview on mesh and scene

- **Wire path:** `vertex_material_canvas_previews_over_mcp_without_saving` sends the editable vertex canvas through `material.graph.preview`, observes the hosted `material.preview.set` request and completed response, and verifies that preview creates no project graph asset. The desktop panel submits the same canvas through `Editor::preview_material_graph` when its semantic graph content changes.
- **Scene compilation:** `authored scene compiles a surface beside its vertex graph` compiles the authored vertex expression for visible, depth, and shadow passes. The hosted mesh pixel case `a compiled vertex offset moves the hosted Metal material mesh` exists, but its pixel assertions require a native Metal device.
- **Green commands:** the focused MCP test passed (1/1), and the authored scene compilation case passed (26/26 assertions) on 2026-09-27. The local hosted mesh case selected the null RHI and executed only its availability assertion. Task 3.4 remains open for a native preview image and individual material canvas transaction parity.
- **Weather wind regression:** `authored scene binds weather wind for a vertex field graph` refuses an unbound `moisture` field, accepts typed `wind`, compiles that scene program to MSL, and renders it on the null device through the authored frame, including a camera move that refreshes the field image. The focused case passed 19 assertions on 2026-09-27. `editor weather publishes the wind image sampled by a scene material` runs the real WeatherSystem and compares its deterministic field image with `FieldStore` at two world origins, three heights, and three horizontal positions: 84 assertions passed. Native pixel evidence remains open because the local Metal device is unavailable.
- **Red mutation:** shifting the field image origin by 256 m after publication made the WeatherSystem agreement test fail 54 of 80 assertions; restoring the origin returned it to green. This detects a coordinate error even when the image buffer itself is valid.
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
- **Scene assignment regression:** `assigned_material_geometry_includes_meshes_and_terrain_layers` creates a material shared by a static mesh and a `TerrainMaterialLayer`, checks the `StaticMesh,Terrain` engine request, checks a terrain-only assignment, and ignores an orphan layer. Temporarily omitting the terrain source made the focused test fail with only `StaticMesh`; restoration passed. The desktop collector also retains imported mesh material slots. The 168 service and 102 shell library tests, all-target Clippy for those crates, and strict OpenSpec validation passed locally.
- **Terrain cook discovery:** `cy_material cook --world` now discovers live `TerrainMaterialLayer` children of `TerrainAuthoring`, merges `Terrain` with any other assignment for the same material, and ignores orphan layers. The saved-world cook regression covers a terrain-only vertex material and an orphan layer; the targeted case passed with 28 assertions and the full `integration.material_cook` suite passed.
- **Remaining work:** virtual-geometry asset discovery and a named editor refusal remain open in task 3.2. A rendered displaced mesh, shadow, and motion-vector comparison against CPU-displaced geometry is tracked by task 3.3.

## Authored scene vertex animation across frames

- **Committed editor sample:** `samples/05b-editor-window/project/materials/issue15_sway.cymatcanvas` was authored through `cy_material author` into its saved `.cygraph` and assigned in `worlds/issue15-sway.cyworld`. Re-authoring to a temporary path produced byte-identical canonical graph source. `cy_material cook <sample-project> <artefacts> --world worlds/issue15-sway.cyworld` discovered and cooked the material as `StaticMesh` with no failed nodes.
- **Scene pipeline regression:** `committed sine sway material cooks and renders in its authored scene` reads those exact files, checks the Engine vertex unit's sine expression, resolves the saved world, and renders it on the null device. It first renders the same world without the cube's material assignment, then checks that the saved scene binds at least two new graph pipelines. The focused test passed 18 assertions. Replacing the saved `material.sin` node with `material.one_minus` failed its sine check (17/18). Rendering the unassigned baseline twice failed the graph-pipeline check (17/18). Both mutations were restored. Native pixels remain open.
- **Scene path:** the authored frame accepts a caller-supplied time for deterministic material animation. `authored scene compiles a surface beside its vertex graph` now compiles a sine/time vertex graph for visible, depth, and shadow stages and checks that previous-frame evaluation subtracts the frame delta. A two-frame native test compares both the temporal image and packed prepass motion with the same mesh translated on the CPU by `4 * sin(0.05)`; motion readback is opt-in.
- **Green commands:** `build/dev/cy_test_smoke_editor_authored_frame_metal --test-case='authored scene compiles a surface beside its vertex graph,authored frame refuses nonfinite material animation time' --no-skip` — shader compilation, source assertions, finite-time refusal, and a null-RHI command-log check of the opt-in motion copy passed on macOS.
- **Red mutations:** temporarily evaluate the previous position at current time in `material_runtime.cpp`; the compile test failed its previous-time assertion (25/26 passed). Temporarily omit the motion copy callback; the null-RHI test failed because only one texture copy occurred instead of two (7/8 passed). Both mutations were restored.
- **Depth-pass time regression:** the PR's generated depth vertex entry already evaluates the current position at `sceneMaterialTime()` and the previous position at `sceneMaterialTime() - sceneMaterialDelta()`. The previous shader-unit check only searched for the subtraction anywhere in the source. The strengthened check locates each expression and asserts its own time argument. Swapping them locally made both checks fail (28/30 assertions passed); restoring the PR's order passed 30/30. Native pixel confirmation remains open below.
- **Long-frame motion:** the authored frame no longer clamps the animation time delta to 0.1 seconds, because the generated depth vertex shader subtracts that delta to find the previous position. The two-frame CPU comparison now advances 0.2 seconds and checks the temporal image and packed motion texture against the CPU-translated mesh. This native pixel case could not execute on the local null-RHI fallback; its build and the shader-unit test passed, and native result remains required for task 3.3.
- **Native pixel status:** the local RHI selected its null fallback, so the two-frame image and motion comparisons did not execute here. Task 3.3 remains open pending a native Metal or Vulkan run.
- **Complexity:** the cognitive-complexity skill measured the changed authored-frame functions after extracting motion capture: `capture` 32, `initialize` 25, `render` 15, `record_motion_capture` 1, and `copy_motion_readback` 4. All are within the systems target of 25–35 for inherently branchy frame setup.

## Two-emitter sample image

- **Source:** `samples/05b-editor-window/project/effects/issue15_two_emitters.cyvfxdoc`.
- **Save and reopen:** `committed_two_emitter_sample_reopens_without_losing_stage_graphs` decodes and re-encodes this exact draft, checking both simulation paths and stage snapshots. `committed_two_emitter_sample_can_be_authored_through_mcp_commands` rebuilds the same engine-relevant graph facts and declarations through MCP, then reopens the saved result. The editor's fresh-window `vfx_draft_saved_through_command_reopens_in_a_fresh_window` test covers the project save/read command path.
- **Cook and preview:** `unit.editor_backend` compiles that exact draft with CPU and GPU emitters; `integration.editor_backend_compile` loads it into the engine preview and inspects live particles.
- **Rendered comparison:** `smoke.editor_authored_frame_metal` advances the engine preview, checks particle publication, and compares its 640×360 frame against `samples/05b-editor-window/runtime/tests/references/issue15_two_emitters_metal.png` using the shared golden-image metric. It also verifies that removing the preview restores the baseline frame.
- **Green command:** `ctest --test-dir build/dev -R '^(unit\.editor_backend|integration\.editor_backend_compile|smoke\.editor_authored_frame_metal)$' --output-on-failure` — 3/3 passed on macOS Metal.
- **Red mutation:** immediately after `render_test::adopt` in the Metal case, temporarily apply `captured.texels[0] ^= 0x00ffffffU`. The test fails with one differing texel away from edges and worst channel delta 255 at `(0, 0)`. Remove the mutation, rebuild, and rerun; 3/3 pass. The mutation is not committed.
- **Scope:** the Metal comparison is verified on this Mac. The Vulkan two-emitter image case is present but its pixel assertion has not run here because no Vulkan device is available.

## Material canvas and save history

- **Regression:** `desktop_material_save_and_mcp_share_one_undoable_transaction` opens an engine-catalogue material canvas with a node, saves through the registered command, checks the engine authoring request and both saved files, then checks that undo clears the canvas and redo restores its node. A later unsaved node remains visible when the project source has not changed.
- **Red mutation:** before material history synchronization, the test failed after undo because the canvas still had one node although the saved graph file had been removed.
- **Green commands:** the focused regression, 99 shell unit tests, 167 service unit tests, all-target shell/service Clippy, Rust formatting, and strict OpenSpec validation passed locally.
- **Scope:** this covers the Save transaction and open-canvas history synchronization. Material edit command coverage is recorded below; native preview-mesh evidence remains in task 3.4.

## Material node placement parity

- **Command:** `material.node.add` reads the saved editable canvas, validates the node against the engine catalogue, places it with the shared canvas model, and requests engine authoring. The saved desktop palette invokes this command when its canvas still matches the project source.
- **Wire regression:** `material_node_add_uses_engine_catalogue_and_undoes_over_mcp` saves an empty canvas, refuses an unknown type, adds an engine-catalogue node, receives the engine's authored result, then checks undo and redo of the editable source. `saved_material_palette_node_uses_the_shared_edit_command` verifies desktop intent routing and local draft behavior.
- **Red mutation:** removing the material command registration from the MCP test registry made the wire case fail at the successful add assertion (`isError` became true). Restoring registration made it pass.
- **Green commands:** 152 interface unit tests, 100 shell unit tests, all their integration suites, 29 MCP wire tests, all-target Clippy for the affected crates, and Rust formatting passed locally.
- **Scope:** node placement has desktop and MCP parity for saved graphs. Additional saved-graph gestures and source-only draft transactions are recorded above and below.

## Material saved-graph gesture parity

- **Commands:** `material.node.connect`, `move`, `property.set`, `disconnect`, and `remove` use the same engine catalogue and shared canvas as `material.node.add`. Desktop controls route saved-canvas gestures to these commands, with a drag producing one move on release. Each command requests engine authoring, which journals the canonical graph and editable canvas together.
- **Wire regression:** `material_node_edits_round_trip_as_individual_mcp_transactions` makes seven saved-graph gestures over MCP, checks their canvas facts and seven separate undo entries, refuses an invalid scalar property without changing source or history, and checks undo/redo of removal. `saved_material_drag_queues_one_move_on_release` checks the desktop drag boundary.
- **Red mutation:** removing `material.node.connect` registration made the MCP sequence fail on the connect call (`isError` changed to true); restoring registration made the sequence pass.
- **Scope:** source-only draft gesture history is recorded above. Native material preview-mesh evidence remains open under task 3.4.

## Desktop VFX creation history

The New VFX system button now invokes the same `vfx.document.create` command used by MCP and
opens the saved source. The shell regression covers create, undo removing the source and closing
the canvas, redo restoring both, and refusal to overwrite an existing source.

The Create module button similarly invokes `vfx.module.create` at the selected Asset path. Its
shell regression covers saved creation, undo and redo of both source and open module, and refusal
to overwrite an existing module. Switching to another module still preserves unsaved edits until
the author saves or discards them.

The headless egui accessibility regression clicks both desktop creation buttons and checks their
exact saved-command intents and project paths. Mutating either button's name argument made that
test fail; restoring each argument made it pass. This covers the UI wiring in addition to the
command-history tests above.

An exploratory typed wind-field shader probe compiled but failed all four assertions for a real
`cy.field` import, a bindless slot, a spatial sample, and a float3 result. It was removed after the
red run: the field image keeps its world origin outside the GPU words, so a shader-only change
would sample the wrong location. The current authored scene now supplies the origin and one field
snapshot to visible, depth, and shadow programs; native rendering verification remains open.

The first task 3.1 implementation now generates typed field reads through `cy.field` with a
bindless slot and camera-to-image offset. The focused `typed vertex fields sample the engine field
table at the authored position` case compiles the generated vertex program through Slang; all 19
assertions pass, and the full `smoke.material_slang` suite passes. The prior scalar fallback made
four assertions fail before the change. The authored frame now binds an Engine wind image. The
field bindings moved to a separate per-frame buffer so the material parameter layout stays stable;
`smoke.material_slang` passes 10 cases and 140 assertions, including the typed wind probe. The
authored scene's headless test compiles the wind material to MSL and draws it on the null device.

Task 3.1 audit: the Engine material catalogue publishes separate surface and vertex roots, a
named interpolant output, scalar normal displacement, typed position/normal/UV/colour/time inputs,
noise, procedural wind, field sampling, and shared numeric math. The editor consumes the Engine
stage mask. Graph/text lowering, typed field Slang compilation, and the scene and material-mesh
shader tests cover these nodes. `unit.graph_material`, `integration.graph_material`, and
`smoke.material_slang` passed locally; the editor-interface material filter passed 10 tests.
The native image and pass-consistency acceptance remains in tasks 3.3 and 3.4.

The hosted material mesh's shader assembler now carries the camera-relative field position in
vertex and fragment contexts for a typed `wind` graph. The focused
`the hosted material shader gives typed wind a camera-relative field position` regression passed;
the full `smoke.editor_material_metal` suite passed 12 cases and 99 assertions on this host. Removing
the vertex field-position assignment made the focused case fail 2 of 10 assertions; restoring it
returned the test to green. Native image cases still stop at the Metal availability check, and
the first-light renderer now binds an Engine weather image before drawing a field graph. The
headless `the hosted wind preview compiles Engine field sampling before device binding` test
compiles the generated wind program through Slang to MSL and reaches only the null device's named
Metal-pipeline refusal. The new native `a weather-owned wind field moves the hosted Metal material
mesh` case verifies a missing image refusal, then compares wind and plain pixels and refreshes at
a moved camera; its image assertions have not run on this host. The bounded field-table null test
passed 6 assertions. Temporarily changing its global table binding count back to two made its
valid write fail (5 of 6 assertions); restoring binding three returned the case to green.
