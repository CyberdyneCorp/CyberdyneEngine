# Add the RTS test map: a cooked map that looks like a game under the Swift RTS

## Why

`samples/13-rts-api` proves a Swift RTS drives the engine — orders, the HUD, a lockstep company,
the GI, the fog of war — and draws all of it over **boxes**: a 32 m slab, a barracks box, unit
boxes. Nothing shows the engine's terrain, foliage, materials, skinning and lighting composed into
a place a player would recognise, and M12's slice (`implement-m12-the-game`: "200 units selected
and ordered across one map") has no map to grow from. The terrain tools (#63), the foliage
placement, the skinned pipeline (#107), the device GI (#127) and the fog of war have each been
observed alone; a map is where they are observed together.

## What changes

- **A map source and its cook.** `content/rts/frontier.cymap`, a line-based source naming the
  terrain's generator and modifier stack (noise, hills, the carved lake, flattened ground, roads and
  the brush strokes the editor's terrain tools record), its painted layers, its materials, its
  foliage species, rules and exclusion zones, and its placements. `cy_rts_map_cook`
  (`samples/13-rts-api/map/`) evaluates the stack with `terrain::evaluate_region`, bakes the painted
  layers with the slope's rock and the shore's sand into an albedo with a mip chain, grows the
  forest with `foliage::generate_region`, builds the kit — buildings, resources, plants and a
  skinned worker on the worker rig's joints — from primitives, and writes ten assets in the
  cooked-asset envelope into a `.cypak` under fixed ids. Two cooks are byte-identical.
- **A loader on the engine's asset path.** `load_map` mounts the package and reads every asset by id
  through `AssetSystem`, refusing a wrong kind, a damaged payload, a newer format and a truncated one.
- **The session drawn on it.** `cy_sample_rts-api --render <dir> --map <package>` draws the Swift
  game on the map with the engine's frame: the standard material and its texture table, the sun's
  shadow map with the soft filter, ambient occlusion, the device GI over the level's system (the
  map's buildings added to it as boxes), blue's fog over the map with the minimap's overlay, the HUD
  the game built, and every unit skinned on the device — the workers posed by the engine's
  animation system, the company and the camp by the rig's clips. The map is presentation: the game's
  report is the headless run's.
- **One assembly cost removed at its root.** `FrameAssembly` integrated the sky's irradiance every
  frame though nothing it depends on had moved; it now reuses it while the atmosphere, the eye's
  height and the sun are unchanged, and reports `AssemblyReport::sky_irradiance_reused` (appended).
- **Tests**: `integration.rts_map`, `smoke.rts_api_map` (gameplay against the headless run, 420
  ticks, a golden still, a 1080p frame within 16.7 ms on this machine's device) and a case in
  `integration.render_assembly`; every one proved red by a mutation (`evidence/`).
- **Docs**: `content/rts/README.md` and `PROVENANCE.md`, `deps/shipped-content.md` and
  `THIRD_PARTY.md`, the sample's README, `docs/guides/rts-map.md`, `just capture-rts-map` and its
  images and video.

## Scope

In: the map, its cook, its loader, its renderer in the sample, its tests and its measurements. Out:
streaming the map, LOD for the terrain and the forest, wind, a transparent water material, a sky
behind the horizon, fog of war over the whole map in the simulation (the grid stays the level's),
and running the map on Metal or D3D12, which compile the frame's shaders and were not run.

## Impact

Additive. The sample's runs without `--map` are unchanged; the one engine change is the sky
irradiance reuse, which returns the value a fresh integration returns. No ABI change.
