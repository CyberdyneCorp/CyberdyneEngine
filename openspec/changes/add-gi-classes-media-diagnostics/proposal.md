# Classify moving objects, light transparent surfaces and fog from the cache, explain the GI, and show it

## Why

Four requirements of `rendering-global-illumination` were the row's last exemptions in
`tools/roadmap/requirements-coverage.toml`, each because nothing in the tree implemented it:

- **Illumination classification of dynamic objects.** `GiScene` ingested surfels per cell and knew
  nothing of what moved: a projectile and a parked car were the same, and nothing rate-limited or
  excluded a renderable.
- **Transparency and participating media.** Nothing distinguished a thin-transmission surface from
  an opaque one, the volumetric fog lit itself with the frame's flat ambient term, and the deferred
  features had no reserved interface.
- **Illumination diagnostics.** There was no per-point record of which probes answered, no leak
  detection, no per-stage cost and no attribution of cost to content.
- **GI debug visualisation.** None of the nineteen GI views existed; `GiProbes` was a name the editor
  could request and no frame drew.

## What changes

- **`gi/classification.h`** and `IlluminationSystem::add_object` / `move_object` / `pose_object` /
  `remove_object`: `Static`, `SlowDynamic`, `FastDynamic`, `TransientIgnored`, derived from mobility
  and the world size, overridable. Static and SlowDynamic objects are cells of their own in the GI
  scene with their own field placement; a SlowDynamic object's moves are coalesced and flushed once
  per interval. FastDynamic objects are proxy spheres — coarse, or the posed bones of a skinned one
  near the camera — that occlude the indirect diffuse in closed form, reflect the cache's light, and
  show in specular rays. TransientIgnored objects are recorded and nothing else.
- **`gi/media.h`**: the staging table, `RefractionSolver` and `CausticsSource` reserved and refused,
  thin transmission through `SurfaceProperties::transmission` and `ResolveResult::transmitted`, and
  `MediaIndirectGrid`. **`fog::FogIndirect`**: the volumetric fog's march, on the host and the device,
  scatters the radiance cache's grid in place of the ambient term when one is given.
- **`gi/diagnostics.h`**: `IlluminationSystem::explain` and `explain_region`,
  `RadianceCache::contributions`, `DarknessCause`, and `GiProfile` with per-stage wall time, rays per
  tier, hit rates and cost attribution. `TracingDiagnostics` gains the screen hits and the tiers' time.
- **The leak the query found**: `RadianceCache::visibility_weight` adds a per-axis test, on the host
  and in the device transcription.
- **`gi/debug_views.h`**: nineteen views drawn by `render_gi_debug_view`; nineteen `DebugViewMode`s
  appended, plus `LightmapCharts`; the forward frame draws `LightmapCharts` and composites a GI view's
  image (`lightmaps::GiDebugOverlayTexture`) through `lightmapDebug`'s reserved word; the editor's
  view-mode list mirrors them.

## Impact

- Code: `src/rendering/gi`, `src/rendering/gi_gpu` (the visibility transcription),
  `src/rendering/fog`, `src/rendering/lightmaps`, `src/rendering/pipeline`, `src/servers/render`,
  `src/rendering/shaders/cy/frame.slang`, `editor/crates/cy-editor-viewport`. The generated headers
  of fog, gi_gpu, the frame, selection, particles and strips are regenerated.
- Frame block: unchanged in size; `lightmapDebug.x` gains two values and `.z` a meaning.
- Off is the frame before: no object, no transmission, no grid and no debug view are bit for bit the
  previous answers, and `render.volumetric_fog` (a), `render.lightmaps` (c) and every render suite
  hold it on the device.
