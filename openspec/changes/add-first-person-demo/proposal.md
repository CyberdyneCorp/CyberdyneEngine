# Add a rendered first-person demo

## Why

Every piece a first-person game is made of has landed on its own — the character controller (#97),
the first-person camera and weapon view (ABI 1.13), lights a game spawns (ABI 1.11), decals (#38),
VFX particles, the AI runtime, the device GI and the temporal upscaler — and each is proved by its
own suite. Nothing played them together, and two of them could not be reached from a Swift game at
all: a game had no way to play a burst of particles where its shot landed or to leave a mark there.
A shot's aftermath is presentation the game decides, so it belongs on the ABI beside the lights.

## What changes

- **ABI 1.15: effects and decals.** `effect_find` and `effect_play` play a VFX system the host
  registered by name, fire-and-forget, with parameter overrides; `decal_find_material`,
  `decal_spawn` and `decal_status` spawn a projected decal into the frame's budget. Playing and
  spawning are refused in a fixed step; resolving a name is not. New adapters
  `cy::game-backend-effects` (over `cy::vfx::SimulationWorld`) and `cy::game-backend-decals` (over
  `cy::rendering::DecalBudget`); CyberdyneKit's `Effect`, `DecalMaterial` and `Decal`; the Rust SDK
  and the Swift overlay regenerated.
- **A spawn with an end.** `vfx::EffectSpawn::spawn_seconds` (appended): an instance stops spawning
  once it has spawned for that long and simulated at least once, so a fire-and-forget burst releases
  itself. Before, a fire-and-forget instance was released only after a `stop` nobody could call.
- **A Jolt world sized below its contact buffer crashed.** The Jolt server's temp arena counted 256
  bytes a contact constraint; Jolt takes `cMaxConstraintSize` for each, for the whole capacity,
  every step. A 128-body world at the default capacities failed that allocation on its first contact
  and crashed. The term is now Jolt's own size.
- **samples/15-first-person.** A Swift game: a character that walks, sprints, jumps and crouches
  (the controller rebuilt shorter, and taller only when a shape test finds headroom), a first-person
  camera with recoil and a weapon view, hitscan shots by physics raycast, impulses on props, damage
  on targets, and — through ABI 1.11 and 1.15 — a muzzle flash, a muzzle burst, sparks that bounce on
  the floor, scorch marks and sounds. The targets are the engine's AI: perception through the
  physics world, a compiled behaviour tree, a hit felt as a perception. The host draws it through the
  engine's frame with the device GI, the upscaler, shadows, ambient occlusion, the decals, the
  particles and the flashes, and holds it to 16.7 ms a frame at 1080p.

## Impact

ABI 1.14 modules run unchanged: the five entries are appended. A host that binds neither adapter
answers UNAVAILABLE from every 1.15 entry. An effect played without `spawn_seconds` behaves as
before. The Jolt arena grows by the difference for worlds with large contact capacities (2048
contacts: about 0.5 MiB).
