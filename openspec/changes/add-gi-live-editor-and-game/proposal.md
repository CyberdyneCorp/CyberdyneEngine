# Global illumination live in the editor's viewport and in a Swift game's frame

## Why

The device GI (`gi_gpu`) lit only the suites' frames and the beauty shot. The editor computed its
GI views on the processor — about 240 ms for the indirect diffuse view at 320x180, a few frames a
second — and its Lit view was never lit by the GI at all; the game host the Swift samples run in
(`samples/13-rts-api`) bound no GI, and a Swift project's game in the editor's Play reached none.
Issue #35 asks for the GI an author tunes to be the GI a player sees, at interactive rates, and
issue #29 for the Swift API to drive it in a running game.

## What changes

- **The device GI as one frame feature** (`gi_gpu::GpuGiFrame`): the scene, the radiance cache,
  the hardware structures where the device has ray queries, the resolve and the budget link over a
  host's own `IlluminationSystem`, attached to an assembled frame in three calls; and
  `FrameAssembly::set_global_illumination` with `AssemblyDescription::normal_prepass`, so a frame
  switches the GI on and off between frames over one set of depth pipelines.
- **The nineteen GI debug views on the device** (`gi_gpu::GpuGiDebugView`): drawn inside the frame's
  `ScreenSpaceGi` stage from the device frame's own state — the resolve's surfaces, published
  targets and history, the probe set, the cards and their lookup, the field — each a transcription
  of `gi/debug_views.cpp`'s function and held to it word for word. A probe record carries its error.
- **The editor**: the runtime lights its viewport's Lit view with the device GI over the preview's
  system when the world's "Global Illumination" node admits it, draws every GI view on the device,
  applies the document's GI settings, volumes, classes and emitter rules to the running GI in place
  (the sky too, now: it is the frame's own ambient), and binds ABI 1.9 in Play so a project's
  Swift game tunes the same GI.
- **The game**: `samples/13-rts-api` hands its level to an `IlluminationSystem`, binds ABI 1.9, and
  — with `--render` — draws the session lit by `GpuGiFrame`; its Swift commander places a GI volume
  over its base and puts a beacon out and lights it again by its material's name.

## Impact

- Code: `src/rendering/gi_gpu` (two classes, four shader entry points, the probe record's error),
  `src/rendering/assembly`, `src/rendering/forward`, `src/rendering/pipeline` (the recorder's prepass
  check), `src/rendering/gi` (two view helpers made public), `src/editor_backend` (accessors, the sky
  applied in place), `samples/05b-editor-window/runtime`, `samples/13-rts-api`,
  `src/rendering/pipeline/tests/frame_scene.*` (hooks a sample shapes its field with).
- ABI: unchanged (1.9).
- Off is the frame it was: a frame with no `GpuGiFrame` attached, or attached under a mode that
  admits no dynamic bounce, is byte for byte the frame without it; the editor's Lit view of a world
  with no GI node is the frame it was.
