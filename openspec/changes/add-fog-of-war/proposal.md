# Add fog of war: the grid, the frame, the minimap and the Swift API

## Why

`gameplay-framework` requires a strategy mode to "compose base strategy, economy, fog of war, and a
victory rule", and M12's slice (`samples/12-rts`, `tools/roadmap/milestones/m12.toml`) names fog of
war as one of the rows it exercises. The tree had the NAME — `unit.gameplay_core` registers a
`fog_of_war` feature contributing a `VisibilitySystem` — and nothing behind it: no grid, no team's
view of a cell, no remembered structure, nothing a renderer could draw and nothing a Swift game
could ask. An RTS built on the engine had to write all of it, and a lockstep RTS had to write it
twice as carefully, because what a team may see decides which orders are legal.

## What changes

- `cy::gameplay-fog` (`src/gameplay/fog/`), a new module: `FogOfWar`, a grid of cells holding one
  bit a team for "seen now" and "ever seen", updated in the fixed step from units' Q32.32 positions
  and reveal radii by INTEGER arithmetic only (a position located to 1/256 of a cell, a cell in
  sight when its centre's squared distance is no more than the squared radius), shared vision
  between allies, and GHOSTS: a structure another team saw is remembered where it was seen until
  that place is seen again. A refused update changes nothing; `digest()` is the same on every
  platform, and `unit.gameplay_fog` holds a committed one.
- `cy::rendering-fog-of-war` (`src/rendering/fog_of_war/`), a new module that READS the grid as one
  state byte a cell and links no gameplay code: `VisibilityField` fades each cell on the frame's
  clock (reveal and hide times) and packs an `Rgba8Unorm` texel a cell; `FogOfWarPass` uploads it as
  the visibility texture and applies it at a new frame stage, `FramePassKind::FogOfWar` — on linear
  HDR after the temporal stage and before depth of field, bloom and exposure — reconstructing each
  pixel's world position from the depth, sampling the texture as four bilinear taps half a cell
  apart (a two-cell tent, smoothstep-eased) and shading: explored ground desaturated and darkened,
  never-explored ground the unexplored colour, visible ground written back bit for bit.
  `compose_minimap_fog` draws the same field as the minimap's overlay image.
- The frame: `FrameFeatures::fog_of_war`, `FrameDescription::fog_of_war_stage`,
  `FrameResources::fog_of_war`, `AssemblyDescription::fog_of_war` (`FrameAssembly::set_fog_of_war`)
  and `FrameSinks::fog_of_war`, every one appended. Off, nothing is declared.
- ABI 1.10 (`fog_set_vision` `[N F]`, `fog_get_vision`, `fog_cell`, `fog_sight`, `fog_status`
  `[N F U]`), reached through `abi::game::FogBackend` and implemented by
  `cy::game-backend-fog`'s `FogAdapter`, which hands the grid its units ordered by entity so the
  digest is the state's and not the call order's. CyberdyneKit wraps them as `FogOfWar`.
- `samples/13-rts-api`: the host keeps the grid and places an enemy camp — two raiders and a
  watchtower — that the Swift game resolves by `@Node` and gives red's sight; the game gives its
  squad and barracks blue's, reads which of the camp blue sees for its minimap, and is refused once
  when it tries to give a vision from a frame. `--render <dir> --fog` draws blue's fog and the camp
  by blue's sight; the HUD's minimap (C++ and Swift alike) shows the fog as an image between its
  ground and its dots.
- Tests: `unit.gameplay_fog`, `unit.render_fog_of_war`, cases in `unit.render_forward` and
  `unit.abi` (`test_game_fog.cpp`, the 1.10 table and layout), `integration.game_backend_fog`,
  `render.fog_of_war`, a case in `render.rts_api_hud`, `smoke.rts_api_fog`, and the fog cases of
  `integration.rts_api_sample`. Every one was proved red by a mutation (`evidence/`).

## Scope

Vision is a radius on a flat grid: there is no line-of-sight occlusion by terrain or structures, no
height advantage and no vision cone. Ghosts are kept for structures (`leaves_ghost`); a mobile unit
out of sight is not drawn and is not remembered. The fog is drawn on surfaces the depth reaches and
leaves the sky as it was. Metal and D3D12 compile the dispatch (the shader set builds for every
target) but were not run: the device suites ran on Vulkan only.
