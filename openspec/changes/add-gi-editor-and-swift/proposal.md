# Author and drive global illumination from the editor and from a Swift game

## Why

CyberGI could be tuned, given volumes, told what moves and asked what lit a point only from C++,
and only by a caller that rebuilt it to change a setting. The editor listed the GI debug views as
view modes its own viewport never drew, and no panel showed `explain()` or the profile; a Swift game
could do none of it. Issue #35's program asks for the editor and the Swift API, and for a GI that a
player's options menu or a level script changes without a visible pop.

## What changes

- **Run-time control of a configured system** (`gi/system.h`): `set_runtime_settings` (mode, global
  quality and tier, the hardware tier's profile switch, the budget), recorded on every live probe in
  place; `set_emitter_rules` (per material, per instance or both: automatic, cache-only, promote,
  off), classifying each emissive cell again in place; `set_object_class`, moving an object's cards
  and its own field into the GI scene and out. Untouched, each is the configured system bit for bit.
  A scaled object's cards carry the area of their scaled surface.
- **ABI 1.9**: ten `gi_*` entries over `cy::abi::game::IlluminationBackend`, implemented by
  `cy::game-backend-illumination`; `[N U]` every one. CyberdyneKit's `Illumination` and `GIVolume`.
- **The editor**: `gi.settings.set`, `gi.volume.create`, `gi.volume.set`, `gi.object.set-class`,
  `gi.emission.set` (undoable, MCP tools), `gi.report` and `gi.status`; the lighting panel's global
  illumination half and the Inspector's class row; a GI volume placed by the move gizmo and sized by
  the scale gizmo. `cy::editor::IlluminationPreview` and `IlluminationService` (`illumination.report`)
  in `cy::editor-backend`; the editor-hosted runtime draws the nineteen GI views over its own frame
  (`GizmoIntent::debug_view`) and the volumes' boxes.

## Impact

- Code: `src/rendering/gi`, `src/abi`, `src/game_backend/illumination`, `src/editor_backend`,
  `src/servers/render` (the gizmo intent's view byte), `samples/05b-editor-window/runtime`,
  `bindings/swift`, `editor/crates`. The Swift overlay and the Rust SDK are regenerated.
- ABI: 1.8 → 1.9, append-only.
- Off is the frame before: the editor's Lit frame is byte for byte the frame before a GI view was
  drawn over it, and no setting, rule or class changes a system nobody called them on.
