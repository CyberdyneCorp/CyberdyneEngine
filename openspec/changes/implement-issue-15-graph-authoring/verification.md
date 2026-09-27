# Verification ledger

This ledger records executable evidence for issue #15. It is incomplete until every acceptance criterion has a green check and a recorded red mutation.

## Two-emitter sample image

- **Source:** `samples/05b-editor-window/project/effects/issue15_two_emitters.cyvfxdoc`.
- **Save and reopen:** `committed_two_emitter_sample_reopens_without_losing_stage_graphs` decodes and re-encodes this exact draft, checking both simulation paths and stage snapshots. The editor's fresh-window `vfx_draft_saved_through_command_reopens_in_a_fresh_window` test covers the project save/read command path.
- **Cook and preview:** `unit.editor_backend` compiles that exact draft with CPU and GPU emitters; `integration.editor_backend_compile` loads it into the engine preview and inspects live particles.
- **Rendered comparison:** `smoke.editor_authored_frame_metal` advances the engine preview, checks particle publication, and compares its 640×360 frame against `samples/05b-editor-window/runtime/tests/references/issue15_two_emitters_metal.png` using the shared golden-image metric. It also verifies that removing the preview restores the baseline frame.
- **Green command:** `ctest --test-dir build/dev -R '^(unit\.editor_backend|integration\.editor_backend_compile|smoke\.editor_authored_frame_metal)$' --output-on-failure` — 3/3 passed on macOS Metal.
- **Red mutation:** immediately after `render_test::adopt` in the Metal case, temporarily apply `captured.texels[0] ^= 0x00ffffffU`. The test fails with one differing texel away from edges and worst channel delta 255 at `(0, 0)`. Remove the mutation, rebuild, and rerun; 3/3 pass. The mutation is not committed.
- **Scope:** the Metal comparison is verified on this Mac. The Vulkan two-emitter image case is present but its pixel assertion has not run here because no Vulkan device is available.
