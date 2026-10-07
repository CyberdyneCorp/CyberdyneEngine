# `src/movement/` — authoritative movement in fixed point

Layer 4 (`scene`), target `cy::movement`. Stage 6 of
[`openspec/changes/add-deterministic-math`](../../openspec/changes/add-deterministic-math/design.md)
(design §8 and §9.1), with the lockstep half of stage 7. Governed by `deterministic-math`,
`navigation` and `simulation-and-determinism`.

How authoritative units move in a `Lockstep` session: positions, velocities and headings in `Fixed`,
collision as circles and capsules on the plane against static navigation and height data and
between units, and the path queries that feed them — all in integer arithmetic, so every
architecture computes the same bits. Physics stays presentation under `Lockstep` (design §8, option
(c)): nothing here reads a physics body.

| Header | What it is |
|---|---|
| `nav_mesh.h` | `FixedNavMesh`: the baked navigation mesh converted once, at load, into a `Fixed` world. `locate`, `closest_point`, `nearest`, `clamp_move`, integer per-polygon blocking, and the refusal of a runtime rebuild as authoritative input |
| `path.h` | A* with `Fixed` g-costs and a wide-square-root heuristic, ties by polygon index; the simple stupid funnel on exact cross-product signs; `follow_path` |
| `flow_field.h` | `FixedFlowField`: a Dijkstra integration of `Fixed` costs over eight neighbours, in (cost, cell) order, with exact unit directions |
| `height_field.h` | `FixedHeightField`: terrain heights cooked to `Fixed16` once, read per tick by a shift, a mask and a bilinear blend |
| `crowd_kernel.h` | `CrowdKernel<Policy>`: integration, the neighbour grid and pairwise separation, written once over a scalar policy |
| `mover.h` | `KinematicMover`, `FixedPolicy`, `MoverParams`, `UnitDesc`, `heading_of` |
| `components.h` | `AuthoritativeTransform`, `publish_units`, `sync_presentation` |
| `determinism.h` | `movement_determinism()`: the mover declares `Lockstep` |

## One tick

```text
desired velocity  (path following, a flow field, or anything else the game decides from commands)
     |
     v
1. integrate    steer toward the desired velocity within the acceleration limit; cap at the speed
2. separate     Jacobi pairwise separation over a grid keyed by Fixed cell coordinates
3. obstacles    push out of static circles and capsules, in the order they were added
4. clamp        stay on the navigation surface (FixedNavMesh::clamp_move)
5. height       cooked height samples, or the polygon's height
6. heading      the yaw facing the velocity
     |
     v
state_hash()    every unit's position, height, velocity, heading and polygon, folded in unit order
```

```cpp
#include <cy/movement/mover.h>
#include <cy/movement/path.h>

cy::movement::FixedNavMesh mesh(allocator);
(void)mesh.convert(baked_mesh);          // once, before tick 0: the one float step

cy::movement::KinematicMover mover(allocator, cy::movement::MoverParams{});
for (const Unit& unit : units_in_entity_order) {
    (void)mover.add(cy::movement::UnitDesc{unit.entity, unit.position});
}
mover.bind(&mesh, &heights);

// Every tick, after the command stream has committed:
for (u32 i = 0; i < mover.size(); ++i) {
    mover.set_desired_velocity(i, cy::movement::follow_path(path[i], mover.position(i),
                                                            mover.max_speed(i), arrival, rate,
                                                            cursor[i]));
}
(void)mover.step(&jobs);                 // the same bits with or without workers
```

## What this module claims

- **Every tick is integer arithmetic.** The mover, the path queries, the flow field and the height
  reads use `Fixed`, `WideFixed` and `Angle` only. Every geometric decision — inside or outside,
  overlapping or touching, which edge is nearer, which side of a funnel — is an exact Q64.64
  comparison, and every tie is broken by an index.
- **Unit order is entity order.** `add()` refuses an entity not above the last, so the order every
  pass relies on is `simulation-and-determinism`'s tie-breaking rule rather than an insertion
  accident.
- **The worker count does not matter.** Every pass but the grid's counting sort writes only its own
  unit's slot and sums its neighbours in a fixed order, so `step(&jobs)` computes the serial step's
  bits (`integration.movement_lockstep`, "the mover on job workers computes the serial mover's bits").
- **The same on every architecture.** `determinism.cross_leg` publishes `detmath-movement-digest`
  (2 000 units, 600 ticks) and the lockstep RTS session's `detmath-lockstep-digest`; each leg checks
  them against committed values on its own, and `cross-leg-compare` compares them under `--detmath`.
- **The float boundary is three files.** `convert_mesh.cpp` and `height_field_cook.cpp` convert baked
  floats once with `detmath::from_f32_cooked`; `presentation.cpp` converts into the scene with
  `detmath::to_f32_relative`, on the presentation side of the tick.

## What it does not claim

- **Off-mesh links** are not converted. A `Fixed` world that needs them declares navigation
  `SamePlatform` until they are, and the profile check names it.
- **Runtime navigation rebuilds** are refused as authoritative input (`FixedNavMesh::check_source`);
  dynamic obstacles in a `Fixed` world are polygon flags set through commands.
- **Avoidance** is separation, not `navigation`'s sampled reciprocal velocity obstacles: the
  `Crowd` in `src/navigation/` stays `f32` and `SamePlatform`.
- **A converted world is one walkable layer** — an RTS map. Multi-storey meshes are not converted
  faithfully.
- **Unit radius at the mesh edge.** The clamp keeps a unit's centre on the surface; a project that
  wants the radius kept off walls bakes its mesh eroded by the radius, as Recast does by default.

## Determinism declarations

| Subsystem | Declares | Where |
|---|---|---|
| `movement` | `Lockstep` | `cy/movement/determinism.h` |
| `navigation` | `Lockstep` when every authoritative world is `NavArithmetic::Fixed` with no runtime rebuilds, else `SamePlatform` | `cy/navigation/determinism.h` |
| `gameplay-commands` | `Lockstep` when the stream runs under a cross-platform profile (payloads checked), else `SamePlatform` | `CommandStream::determinism_declaration()` |
| `abilities`, `ai-utility`, `root-motion` | `SamePlatform`, explicitly | their own `determinism.h` |

`unit.movement`'s profile cases put them together with the real `from_build()`: a `Lockstep` session
of the mover, a `Fixed` navigation world and a checked command stream is accepted, and a `Float`
world, an unchecked stream or authoritative abilities is refused by name.

## Tests and benchmarks

| Suite | What |
|---|---|
| `unit.movement` | The conversion, the queries, the paths, the flow field, the heights, each mover pass, the authoritative transform and its sync, the profile check |
| `integration.movement_lockstep` | `tests/rts_scenario.h`: two peers driven by one command log agree on every tick; a peer missing one command diverges; job workers against one thread; the committed digest |
| `determinism.cross_leg` | The movement and lockstep digests, published for the four-leg comparison |
| `benchmarks/movement/` | 100 000 units: the crowd kernel in `Fixed` against the same kernel in `f32`, the whole tick on one thread and on eight workers (design §11) |
