# `src/gameplay/play/` — layer 4: spawning, and play mode

`SpawnService`, and `PlaySession`: the authored world simulated, and put back exactly.

**Governed by**: `gameplay-framework` (→ Seed at M8.a). Section 5 of the M8.a tasks.

## Why this is a separate target from `cy::gameplay`

Read `src/gameplay/CMakeLists.txt`'s own header first — "READ THE DEPENDENCY LIST BEFORE YOU EXTEND
IT". `cy_gameplay` depends on `cy::ecs` and the core and nothing else, and that is load-bearing:
`tests/test_bypass.cpp` checks with `__has_include` that an input header is not on a gameplay
translation unit's path at all.

A play session needs `cy::scene` for the tree it spawns into, `cy::scene-serialization` for the
authored `.cyworld`, and `cy::physics-bridge` for the bodies. Adding those to `cy_gameplay` would put
four modules' headers on the include path of a suite whose whole argument is about what is *not*
reachable from it. So the dependency goes the other way: this target depends on `cy::gameplay`, and
the module above keeps its short list unchanged. `cy::servers-input` is not here either.

## What is here

| | |
|---|---|
| `spawn.h` | `SpawnRequest`, three policies, reservation, spawn points as metadata, batch spawning |
| `session.h` | `PlaySession`: `enter`, `tick`, `pause`, `resume`, `stop`, and the report |

## `stop()` restores exactly, and here is how that is kept rather than claimed

Task 5.2: *"no residue in the document, the scene or the persistence overlay. A play session that
leaves any makes undo a lie."* Three mechanisms, in increasing order of what it would cost to fool
one:

1. **Play writes ONE thing.** The only mutating call into the authored world during a session is
   `set_transform`. There is no other in `session.cpp`.
2. **The pre-play placement of every node is kept**, and `stop()` writes each one back — the value
   that was there, into the same field of the same component.
3. **The whole file is snapshotted at `enter()` and rewritten at `stop()`, and the two byte strings
   are compared.** `PlayReport::restored_exactly` is that comparison and `restored_difference` is the
   first offset at which they differed. A future change that starts writing something else during
   play turns it false on the first run.

When the comparison fails the session re-reads the snapshot over the world — the total restore — and
reports that it had to. A caller seeing `restored_exactly == false` has found a defect **and** has a
correct world, which is the right way round.

`tests/test_play.cpp` compares the strings itself rather than reading the report and trusting it, and
runs a session twice over one world to show that the second run reproduces the first — which it
cannot if the first left a velocity, a body, a node or a nudged placement behind.

## The physics components are read by NAME out of the file's own type section

`resolve_against` matches a `.cyworld`'s declared names against the engine's `AuthoringSchema`, which
is built from the reflection registry — and physics' eight components are registered with
`register_builtin`, by name, with no reflected type behind them. So a `RigidBody` in a world file
resolves to no `reflect::TypeId`, exactly as `MeshRenderer` does for `cy_editor_services::primitives`.

It is read by name instead, out of the `type` section that `serialization-and-prefabs` requires
precisely so that data a build does not know survives a round trip. **That makes four type names and
six field names a contract across a process and a language boundary**, and it is pinned from both
ends: `kRigidBody`/`kFieldMass`/… in `src/session.cpp`, `BodyBinding`/`ColliderBinding` in
`editor/crates/cy-editor-services/src/bodies.rs`, a whole golden `.cyworld` in `tests/test_play.cpp`
and the same strings in `a_body_is_a_transaction.rs`.

The day those components are reflected, `attach_physics` becomes a lookup through `engine_type` and
nothing else changes.

## What `gameplay-framework` asks for that is not here

**Seed, and the gaps are named rather than implied.**

* **Five of the eight spawn policies.** Region, weighted random, formation, navigation-reachable and
  authority-assigned each need something this milestone does not have — a region volume, a
  spawn-weight surface, a formation description, a navmesh (M8.b) and a network authority (M9). The
  seam is `SpawnPolicy` and `select()`, and adding one changes neither the request nor any call site.
  A policy this build cannot honour is REFUSED by name rather than silently falling back.
* **Batch spawning is one call and is not yet one operation.** `SceneTree::create_node` makes a name
  unique among its siblings by scanning them, so N instances of one name cost O(N²) comparisons:
  two hundred measured **9.4 ms of CPU at -O2**, nine times a unit test's whole budget. The
  requirement's ten thousand would not finish in a frame. Closing it is a naming scheme that does not
  scan plus `World::instantiate` over the template's archetype block.
* **Session state, rules, participants and teams during play.** A `PlaySession` is the *play mode*
  half of the session model — enter, tick, stop, restore. `GameSession` and `WorldSession` are
  `cy::gameplay`'s and are not joined to it yet; a session that spanned a lobby and a play world is
  the thing that would join them.
* **Time domains.** The session runs one `SimulationClock` in `FixedStep`. Pausing is a state on the
  session rather than a per-domain policy.
