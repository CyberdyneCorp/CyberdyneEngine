# CyberGI for games: live caches on the device, streamed with the world

## Why

The GI's device half resolved a frame in 2.7 to 3.8 ms, but the figures were taken with the host's
caches held still: the beauty shot kept its surface cache and radiance cache current on the
processor, 15 ms a frame in the Colonnade and 25 to 31 ms in the sunlit room and the workshop hall
(guide section 10), and `GpuGiFrame`'s device caches — on in the editor and the RTS sample — still
paid a host update that sorted every card and probe, rescanned every probe cell and brick, and
rebuilt the device's card lookup from every card whenever one moved. A game also streams its world,
and nothing took a cell's GI content in or out as cells activate. A small promoted lamp lit the
frame only through the bounce, because the forward frame's lights did not carry it. And in the
editor's Play the GI was the document's: bodies the simulation moved did not move their GI objects,
and an edit during Play replaced the volumes the game had placed (#127's gaps). Issue #35 asks for
CyberGI finished for games.

## What changes

- **The caches' per-frame work, on the device.** `gi_gpu::GpuGiCaches` owns the card shading and
  installs it and the probe gather through the system's seams; `GpuGiFrame` and the beauty shot both
  run it, by default. The card shade is a workgroup a card, its rays across the lanes and combined
  in the host's order (the same bits, nine times faster). On the host, what is left is the
  decisions, made linear in the caches or skipped when nothing they read changed: the budgeted
  selection a partition rather than a full sort; the field's scroll and the probe placement skipped
  for a still window; the card snapshot not taken when the device shades; the counts made lazily;
  the device's card lookup grid re-listing only the cards that moved; a brick's solve sampling only
  the placements that can be its least. Each shortcut is held to the full computation it replaces.
- **Live measurements.** The beauty sample times frames with the caches live — the GI's sun turning
  and a 3 m block moving through the GI (`gi live`) — and `--gi-caches host|device` keeps the host
  path, the oracle, to compare against.
- **GI content streams with world cells** (`cy::rendering-gi-streaming`, `GiCellStreaming`): a
  `world::CellEventQueue` consumer that ingests an active cell's cards (in chunks, emitters whole),
  distance fields, hardware-tier instances and GI volumes a budget a tick and evicts them with the
  cell.
- **GI volumes by owner.** `set_volumes(owner, volumes)`: the author's, the game's and the streamed
  cells' sets are placed and replaced independently, and only volumes that changed re-place probes.
- **Promoted lamps in the frame's own lights.** `gi_gpu::append_promoted_frame_lights` and
  `render::LightDescription::cosine_falloff` (`kGpuLightEmitter`, `kLightEmitter`): a promoted card
  as a spot of its flux falling as the cosine, which the frame shades as the GI shades its cards.
- **The editor's Play.** Bodies the simulation moves move their GI objects
  (`AuthoredIllumination::follow_play`) and go back at Stop; an edit applied in place keeps the
  game's volumes (they are the game's own set), and an edit that rebuilds the GI binds the same
  adapter to the new system and places them again (`IlluminationAdapter::rebind`, `restore`).

## Impact

- Code: `src/rendering/gi`, `src/rendering/gi_gpu`, `src/rendering/gi_streaming` (new),
  `src/rendering/lighting` and `src/rendering/shaders/cy/light.slang` (the emitter light kind),
  `src/servers/render` (one appended field), `src/game_backend/illumination`, `samples/12-beauty`,
  `samples/05b-editor-window/runtime`.
- Shaders: `gi_cards.slang` and the light library; the committed SPIR-V and MSL headers regenerated.
- ABI: unchanged (1.9).
- Off is the frame it was: a frame with no promoted light, no GI attached, or the GI under a mode
  that admits nothing draws the same bytes (the render suites' committed references, unchanged).
- Verified on Vulkan (RTX 5060) only; Metal and Direct3D 12 compile every shader and run nothing.
