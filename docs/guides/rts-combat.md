# RTS combat and the topographic minimap

A battle fought in the lockstep path, read by a Swift game and drawn where its fog of war lets it see,
and a minimap that shows the ground as a map does. `add-rts-combat-and-minimap`, ABI 1.16;
`samples/13-rts-api --battle` is the end-to-end example.

![The battle from the player's camera at frames 180, 330, 960 and 1150: the armies closing, the
melee with arrows and catapult shot, the watchtower's fall, the barracks burning](../design/images/rts-battle.png)

[The video](../design/videos/rts-battle.mp4) is the whole battle from the player's camera.

| Where | What |
|---|---|
| [`src/game_backend/lockstep/`](../../src/game_backend/lockstep/) | `lockstep_combat.h`: the battle inside `LockstepSession`; `LockstepAdapter`'s event history |
| [`src/abi/README.md`](../../src/abi/README.md) | ABI 1.16's six entries and two order kinds |
| [`bindings/swift/`](../../bindings/swift/README.md) | CyberdyneKit's `Lockstep.arm`, `fortify`, the attack orders, `combatant`, `structure`, `events(from:)`, `projectiles` |
| [`src/rendering/minimap/`](../../src/rendering/minimap/README.md) | `cy::rendering-minimap`: the bake, the fog over it, the icons |
| [`src/ui/render/`](../../src/ui/render/README.md) | `UiRenderer::stage_atlas`: a page updated inside the frame |
| [`samples/13-rts-api/`](../../samples/13-rts-api/README.md) | `game/Battle.swift`, `host/battle.h` (the C++ twin), `host/battle_host.*`, `host/battle_effects.*`, `map/map_minimap.*` |

## 1. Arming a session

A lockstep session (ABI 1.8, [deterministic math](deterministic-math.md)) moves units from a command
log. To fight, a game configures it before its first tick:

```swift
let unit = try Lockstep.enlist(.init(group: 0, entity: soldier, position: at))   // onCreate
try Lockstep.arm(unit, .init(
    team: 1, health: 120, armour: 3, weapon: .melee, damage: 16, cooldown: 36,
    range: Fixed(raw: Fixed.oneRaw * 5 / 4), sight: Fixed(8)))
let tower = try Lockstep.fortify(.init(
    team: 2, position: FixedVec2(x: Fixed(44), y: Fixed(4)), radius: Fixed(raw: Fixed.oneRaw * 5 / 2),
    health: 400, armour: 3))
```

Every number is an integer or a `Fixed`: health and damage are points, cooldowns are ticks, reaches,
sight, speeds and splash are metres in Q32.32. `arm` and `fortify` are refused once the session has
ticked, as `enlist` is; a ranged weapon needs a projectile speed. **Reaches are gaps between
footprints**, so a 0.5 m radius unit and a 2.5 m radius tower are attacked from the same distance
off their edges.

> **Pitfall: melee reach and the crowd.** The session's crowd keeps two half-metre units' centres 2 m
> apart. A reach shorter than the 1 m gap that leaves would see two fighters pushed out of each
> other's reach every tick and chase forever. The battle's swordsmen reach 1.25 m.

## 2. Orders

```swift
try Lockstep.order(.attackMove(group: 0, to: FixedVec2(x: Fixed(34), y: Fixed(7))))  // a step
try Lockstep.order(.attack(group: 2, at: towerPosition))
```

An **attack-move** paths to its point and fights what comes within sight on the way, then takes its
way up again. An **attack** goes for the enemy standing nearest the point — within 3 m of its
footprint, or it is an attack-move to the point — and stays on it until it falls, whatever else is
near: what a player's right-click on an enemy does. Both are the same command as a move, a kind and a
raw `Fixed` point, so they travel in the command log every peer executes. A plain **move** ignores
enemies until it arrives; **stop** stands.

## 3. What one tick does

After the tick's orders execute and before the units move:

1. **think**, every armed, living unit in index order: count its cooldown down; keep its target while
   it stands, or acquire the nearest enemy within its sight (units before structures, the lower index
   on a tie); within its reach, stand and strike — a melee blow lands now, a ranged weapon launches a
   projectile — and otherwise chase it, planning a path every 15 ticks.

After the mover's step:

2. **fly**, every projectile in launch order: home on its target's new position (or fly on to where
   its target fell) at its speed; one that reaches its aim lands — it damages its target, or with a
   splash every enemy whose footprint the radius touches, collected before any damage so a death
   cannot change who else is hit.

Damage is `max(1, damage − armour)`. A unit whose health reaches zero dies on that tick: it stops,
leaves the crowd and can no longer be targeted. A structure falls. The combat state is folded into
the state and world hashes only when something is armed or fortified, so a session nothing is armed
in hashes exactly as before — the company's committed digest is unchanged on every CI leg.

## 4. Reading what happened, and drawing it where blue sees

The tick records events: **strike**, **launch**, **impact**, **damage**, **death**, **destroyed**,
numbered from zero. The adapter keeps a bounded history after every tick (4096 by default), so a
game reads them from its frame update — where effects, lights and sounds may play — with a cursor:

```swift
for event in try Lockstep.events(from: cursor) {          // onUpdate
    cursor = event.sequence + 1
    let world = FixedVec2(x: originX + event.position.x, y: originZ + event.position.y)
    guard try FogOfWar.isVisible(team: blue, at: world) else { continue }   // nothing in the fog
    switch event.kind {
    case .launch: try launch.play(at: muzzle, parameters: [aim.x, aim.y, aim.z])
    case .impact: try explosion.play(at: point, parameters: [-point.y])      // the ground's height
    ...
    }
}
```

`game/Battle.swift` plays a launch's puff and a bowstring or a catapult's thump; a blow's sparks and
clash; an arrow's sparks, bouncing off the ground at the height the game passes; a catapult shot's or
a falling building's explosion with a light that dies in half a second (`Light.spawn`, ABI 1.11),
smoke and a boom; a death's fall. Every projectile in flight leaves a trail on its arc
(`Lockstep.projectiles`), and a building below half its health smokes. **Nothing played feeds back**:
a resimulated tick changes no hit. A fallen soldier's entity is destroyed from the next fixed step,
so it stops seeing for blue.

## 5. The C++ twin and the four legs

`samples/13-rts-api/host/battle.h` states the battle — the squads, their arms, red's two buildings,
the plan of thirteen orders — and `run_battle()` runs it as an issuer and a follower driven by its
log. `determinism.cross_leg` publishes its digest as `detmath-battle-digest` on Linux x86-64 and
arm64, macOS and Windows, each leg checking the committed `kBattleDigest`, and
`tools/ci/cross_leg_digests.py --detmath` compares the legs. `integration.rts_api_sample` holds the
Swift-driven battle to the same digest and to the twin's damage, survivors and events, point for
point. So the battle a Swift game fights is the C++ one, bit for bit, on every architecture.

> **Pitfall: a field nobody reads is still hashed.** The first Swift run disagreed with the twin by
> its world hash alone: a melee spec in C++ carried a default projectile speed of 16, the ABI's an
> unset zero. Behaviour was identical; the hashes were not. A melee spec's speed is zero now, so a
> spec reads the same however it came.

## 6. The topographic minimap

![The minimap at frame 1150: the battlefield explored and seen, the ruin of the watchtower, the
barracks burning, blue's soldiers; the level's workers and the camp below](../design/images/rts-battle-minimap.png)

`cy::rendering-minimap` bakes a height field once — the painted colours, hillshading from the top
left, a contour every interval with an index line every fifth, water by depth and its shore, roads —
then draws a team's fog over it with the frame's own sampler (visible as baked, explored grey,
never explored black), redrawn only over the cells that changed, and stamps icons. A map hands it
its heights, its albedo averaged under each texel and its road layer
(`samples/13-rts-api/map/map_minimap.h`). In the sample the host composes it every sixth frame
with the battle's units — blue's always, red's only where blue sees them — and red's buildings where
blue has explored, and **stages** it into the HUD's page with `UiRenderer::stage_atlas`, inside the
frame.

> **Pitfall: `upload_atlas` while the game runs.** It makes a texture, submits a graph and waits for
> the device every call — a 4 ms stall every update on this host. Use it to make a page, and
> `stage_atlas` to change it.

## 7. Measured

On the RTX 5060 at 1920×1080, Development, no validation layer, `smoke.rts_api_map`'s battle case —
forty units fighting, up to fourteen projectiles in flight and 1 280 particles, red's buildings
falling:

| | |
|---|---|
| a frame, recorded, submitted and waited for | **11.95 ms** on average over 1 186 frames (budget 16.7 ms); the worst 18.1 ms |
| the device's half | 8.6 ms, of which the GI 4.2 ms and the fog 0.07 ms |
| the game's frame (Swift and the host) | 3.9 ms |
| the minimap's bake | 2.6 ms, once |
| a minimap composition (fog redrawn where it moved, icons) | 0.4–1.0 ms, every sixth frame |

Headless, the 1 200 ticks of the battle run in about three seconds.

## 8. The suites

| Suite | What it holds |
|---|---|
| `integration.game_backend_lockstep` | a duel's every blow, tick, point and death; a flight's landing tick; homing; splash on enemies and never friends; a building's armour and fall; the two orders; two peers through the ABI; a follower armed differently; the history outrun; what arming refuses |
| `unit.abi` | the 1.16 thunks' phases, checks, sizes and layouts |
| `LockstepCombatTests` (Swift) | the CyberdyneKit facade's raw bits both ways |
| `determinism.cross_leg` | the battle's committed digest on this leg; a battle given no orders ends elsewhere |
| `integration.rts_api_sample` | the Swift battle's digest, damage, survivors and events against the twin; events read once; drawn by sight; no behaviours, no battle |
| `unit.render_minimap` | the contour rule at every texel; hillshade, water, roads, fog, icons; the incremental fog equal to the whole |
| `render.ui` (j) | a staged page shown in the next frame, one device frame for it |
| `integration.vfx` | the slot-reuse regression the battle found |
| `integration.rts_map` | the battle on the field's flat ground; the minimap's contours against the cooked map's heights; the minimap through blue's fog |
| `smoke.rts_api_map` | the battle drawn with validation on at 480×270, and at 1080p within 16.7 ms a frame |

## 9. Not built yet

* Units do not turn to face a target before they can strike; the presentation turns the figure.
* A projectile is a point on the plane: its arc is the presentation's, and terrain does not stop it.
* Structures are circles; a rectangular footprint is not modelled.
* The minimap's fog and icons are composed on the host; there is no device path.
* With the minimap updated through `upload_atlas` every sixth frame, the battle on the RTS test map
  lost the device on this host at a fixed frame (NVRM Xid 109, a context-switch timeout), with the
  GI, ambient occlusion and the sun's shadows all on. No validation, synchronisation-validation or
  AddressSanitizer finding preceded it; it did not occur under AddressSanitizer or without any one
  of the three. The battle uses `stage_atlas`; the finding is open.
