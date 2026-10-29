# Tasks

- [x] Reproduce the device loss: `samples/13-rts-api --minimap-upload`, the battle's minimap re-made with `upload_atlas` every composition; the frame after the eighteenth upload loses the device on every run.
- [x] Bisect it on the device — by period, by what an upload does, by retirement timing and by pass, with breadcrumbs — and record the table in design.md.
- [x] `UiRenderer::upload_atlas` without a device frame: make the page, stage its texels into the next frame's graph, import a page never copied into as undefined, re-make staging at a new size.
- [x] `integration.ui_atlas_upload` on the null device, red on the old code; record the mutation in `evidence/`.
- [x] Run the battle with `--minimap-upload` with and without validation, compare its stills with `stage_atlas`'s, and run the affected suites one at a time.
- [x] Mark the finding resolved in `docs/guides/rts-combat.md` and `add-rts-combat-and-minimap`'s design; correct the UI renderer's README, the fog-of-war guide and the header's contract; validate this change.
