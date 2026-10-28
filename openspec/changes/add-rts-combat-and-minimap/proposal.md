# Add RTS combat in the lockstep path, and a topographic minimap

## Why

`samples/13-rts-api` plays an RTS on The Frontier (#133, #134) with a fog of war (#132) and a
lockstep company that only walks (#115). An RTS fights. Nothing in the engine could: the lockstep
session moved units and nothing else, so a Swift game had no deterministic way to deal damage, fire
a projectile, kill a unit or knock down a building, and the effects, lights and sounds that make a
battle readable had nothing to be driven by. The HUD's minimap was a coloured panel with dots: it
showed neither the map's ground nor its hills, water or roads.

## What changes

- **Combat in the lockstep session.** `LockstepSession` learns health, armour, melee and ranged
  weapons, projectiles that home and land, splash, structures that block movement and fall, and two
  orders — attack-move and attack — in integers and `Fixed` (`cy/game_backend/lockstep_combat.h`).
  Its state is folded into the session's hashes only when something is armed, so a session nothing
  is armed in runs, hashes and digests exactly as before.
- **ABI 1.16** appends `lockstep_arm`, `lockstep_fortify`, `lockstep_combatant`,
  `lockstep_structure`, `lockstep_events` and `lockstep_projectiles`, two order kinds and their
  types. The adapter keeps a bounded, numbered event history a game reads from a cursor.
  CyberdyneKit's `Lockstep` gains `Arms`, `StructureDesc`, `Combatant`, `Structure`, `Event`,
  `Projectile` and the two orders; the Swift overlay and the Rust SDK are regenerated.
- **`cy::rendering-minimap`**: a topographic minimap baked once from a height field — the painted
  colours, hillshading, contour lines at an interval, water by depth, its shore, roads — with a
  team's fog drawn over it through the frame's own field sampler, redrawn only where the field moved,
  and units and buildings stamped as icons.
- **`UiRenderer::stage_atlas`** replaces an atlas page's texels inside the next frame's graph, with
  no submission and no wait of its own; `upload_atlas` stalls the device for every update.
- **A VFX defect found by the battle**: a retired instance's slot was reused by any system with the
  same emitter count, though its alive flags and parameter words were sized for the first system;
  a larger system overran its neighbour's. A slot is now reused only by a system it fits.
- **samples/13-rts-api `--battle`**: the Swift game fights a battle of forty units and two buildings
  on the company's field through the lockstep path, reads what happened, and draws it where blue's
  fog shows it — launch puffs, trails, sparks that bounce off the ground, explosions with a light,
  smoke, a fall — and hears it. `host/battle.h` is its C++ twin, published by `determinism.cross_leg`
  as `detmath-battle-digest` on all four CI legs. The map renderer draws the soldiers striking and
  falling, red's buildings damaged and ruined, the particles and lights, and the topographic minimap
  on the HUD.

## Impact

ABI 1.15 modules run unchanged: the six entries are appended, and the order kinds and types are
new. A backend built before 1.16 answers UNSUPPORTED from the combat entries. A lockstep session that
arms nothing digests as before (the company's committed digest holds on every leg). The sample
without `--battle` is the sample before, its golden still byte-identical. A VFX world that plays one
system per slot is unchanged; one that mixed systems with the same emitter count no longer corrupts
a neighbour.
