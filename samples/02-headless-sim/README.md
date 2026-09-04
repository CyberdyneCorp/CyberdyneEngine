# `samples/02-headless-sim` — the M2 milestone artefact

One program that authors a scene, cooks it into archetype blocks, loads it into a world, ticks
10,000 fixed steps, and prints a hierarchical state hash that reproduces exactly on a re-run and
after a snapshot restore. Tasks 5.1–5.3.

```
just run-sample headless-sim                     the defaults: 10,000 ticks, 64 instances
just run-sample headless-sim --ticks 100         a short run
just run-sample headless-sim --seed 7            a different session seed, hence a different hash
just run-sample headless-sim --show-scene        print the authoring text form and exit
just run-sample headless-sim --help
```

A run says what it did and exits zero:

```
02-headless-sim: authored prefab=4 entities  placements=2  parameters=1  text=1501 B digest=14c9020918cfd540
02-headless-sim: resolved entities=8 overrides=1 parameters=4 conflicts=0
02-headless-sim: cooked   blocks=2 retained=4 flattened=2 references=1 dangling=0 payload=448 B
02-headless-sim: world    instances=64 entities=530 archetypes=11 chunks=14 fill=23.15%
02-headless-sim: nodes    scene=1 nodes=16 batteries=3 turrets=4 systems=2
02-headless-sim: tick     frames=2500 ticks=10000 epoch=0 tick=10000 version=10000 alpha=0.000
02-headless-sim: hash     b90f811d514640af  archetypes=7 entities=530 components=1170 fields=6580
02-headless-sim: schema   subjects declared=4 undeclared=13 nodes=8288
02-headless-sim: level    world     emplacement              b90f811d514640af  children=7
02-headless-sim: level      archetype archetype                413e83facc588f6e  children=128
02-headless-sim: level      archetype archetype                fdf3cd1495027253  children=128
02-headless-sim: level      archetype archetype                56514ae665277fa7  children=128
02-headless-sim: level      archetype archetype                9ace57caa05a7d54  children=128
02-headless-sim: level      archetype archetype                021ff664926d9050  children=1
02-headless-sim: level      archetype archetype                b3ee7445b5c83705  children=12
02-headless-sim: level      archetype archetype                5d26255319de0595  children=5
02-headless-sim: facade   /emplacement/field/battery-1/turret-2 reads the entity's own transform: yes
02-headless-sim: restore  settled=b90f811d514640af +128 ticks=e6459471feec947e restored=b90f811d514640af  snapshot=68362 B over 530 entities
02-headless-sim: restore  the world moved: yes   the restore reproduced it: yes
02-headless-sim: exit 0 (clean)
```

The whole run is about fifty milliseconds.

## Why this program and not four

M2 built four things — an ECS, a node façade over it, a serializer and cooker beside it, and a
fixed-tick loop with a commit boundary — and each has a suite of its own that passes against a world
it built for itself. What none of them can test is the seam: a `reflect::TypeId` written by a
document's writer, resolved by a cooker, bound to an `ecs::ComponentTypeId` by a template, and read
back by a state hash that has to agree with all three. This sample is one program in which all of
that has to line up, which is why it is the milestone's gate rather than a demonstration.

Three files, one phase each.

| File | What it owns |
|---|---|
| `main.cpp` | The host: the options, the order the simulation and the runtime come up in, the loop, and the report. |
| `content.cpp` | The content: the reflected descriptors, the authored prefab and scene, the text round trip, the cook, the spawn, and the node hierarchy. |
| `simulation.cpp` | The simulation: the state schema, the two systems, and the snapshot check. |

## The six things it actually proves

**A scene survives its own authoring format.** The prefab and the scene are written to the text form
and **read back**, and everything after that line works from what the reader produced — the cook
never sees the documents the program authored. `--show-scene` prints what a designer would edit: one
entity per block, one field per line, values addressed by `TypeId` and `FieldId` and never by name
or by position. A round trip nothing depends on is a round trip that can quietly stop working.

**The flattening rule is a walk to the root, and the numbers say so.** The turret has a `Skirt`
welded to a `Base` that never moves, and a `Muzzle` welded to a `Yaw` that does. A per-edge test
would flatten both — neither child ever moves *relative to its parent* — and the muzzle would then
stay put while the turret turned. The cook reports `flattened=2 retained=4`: two skirts baked, four
relationships kept, over two placements. This is the spike's finding D as a number a test asserts on.

**A cooked reference is fixed up per instance, from a table rather than from reflection.** The
muzzle points at its own yaw. `Link` declares the byte offset of its `Entity` field at registration,
so the cook emits one reference site and the spawn rewrites it as a pass over a known column at a
known offset. `references=1 dangling=0` is that table; sixty-four instances each end up pointing at
their own yaw and not at instance zero's.

**A node's transform is the entity's component.** The `facade` line reads
`/emplacement/field/battery-1/turret-2`'s world transform twice — once through `Node`, once through
`World::get` — and compares them exactly, component by component, with no tolerance. They agree
because there is only one copy: design.md §3's "no shadow copy, no sync step" is checkable exactly
because there is nothing to compare a node against except the entity it is a handle onto.

**Every draw is seeded and every clock reading is the simulation's.** The `drift` system takes its
step and its point from `Simulation::clock()`, which is handed out by `const&` and has no member
that reads a wall clock, and its jitter from a stream named at startup, drawn by
`(point, entity, index)`. There is no generator with a hidden counter, so running the two systems in
parallel cannot reorder anything. `--seed 7` produces a different hash over byte-identical content,
which is the check that the seed is actually reaching the simulation.

**The hash reproduces.** `tests/smoke/test_headless_sim.cpp` runs the whole program twenty times, in
twenty processes, and requires every line of the report to be identical — a fresh address space each
time, which is what would expose a hash that depended on an allocation address or a per-process hash
seed. The `restore` line is the second claim: the world hashed one way, moved when it was ticked
further, and hashed the same way again once the snapshot taken before those ticks was restored.

It also reproduces **across the four profiles**, which the test set does not assert and which is
worth recording because it is the interesting result: `debug`, `dev`, `profile` and `release` are
-O0 through -O3 with assertions on in two of them and compiled out in the other two, and the same
200-tick run hashes `15056a40e210b94f` in all four. A float-contraction difference between
optimisation levels would have shown up here as four numbers. Each profile's smoke test compares
within itself, so nothing gates this; it is measured, not claimed.

## What is thinner than it looks

**The hash covers 4 subjects and is silent about 13.** `schema subjects declared=4 undeclared=13` is
the honest half of the number above it. `ecs::Parent`, `ecs::Children` and eleven of the scene's
twelve built-in components are registered by name with no `reflect::TypeInfo` behind them, so nothing
can derive a schema for them; hashing their bytes anyway is on `simulation-and-determinism`'s
forbidden list. The sample closes two of the gaps by hand — `sample::Placement` and
`scene::LocalTransform` are declared field by field with `StateSchema::declare()`, which is the route
those modules' READMEs name and this is its first caller — and reports the rest rather than
pretending. It closes properly when the ECS's and the scene's headers are annotated and given
manifest identifiers.

**`Placement` is one opaque field to reflection.** M1's reflection has no vector kind, so a
`cy::Transform` member is a forty-byte run it calls `Unsupported`, which is deliberately not
hashable. The ten floats inside it are declared explicitly, by offset, in `simulation.cpp`. That is a
gap in the reflected type system, not in the schema, and one `Vec3`/`Quat` field kind closes it.

**"Reproduces after a snapshot restore" is the round-trip claim, not the replay claim.** What is
checked is that a capture and a restore reproduce the world's state exactly. What is *not* checked —
and is not spellable through `Simulation` today — is re-running from a restored snapshot and getting
the same trajectory: that needs the clock rewound to the same epoch and tick, and the only public
door is `reset_epoch()`, which by design enters a **new** epoch. The epoch is mixed into every random
draw, so a replay under a new one is a different simulation and is meant to be. Rewinding a session
to replay it is M9's, and the seam it needs is a public `SimulationClock::resume()` on `Simulation`.

**The component descriptors are hand-written.** A `reflect::TypeInfo` is plain constexpr data and the
generator emits exactly this shape, but the generator's annotated-header list lives in
`src/core/reflect/CMakeLists.txt` and the identifiers come from `identity/manifest.toml`, neither of
which a sample owns. `src/ecs/tests/fixtures.h`, `src/scene/serialization/tests/fixtures.h` and
`src/runtime/probe/tick_loop_probe.cpp` all record the same seam. Identifiers here start at 9400.

**`alpha` is always zero, and that is what fixed-step mode means.** The interpolation alpha is the
accumulator's residue as a fraction of one step; in `TickMode::FixedStep` the loop runs exactly
`--ticks-per-frame` ticks per frame and there is no residue. The number is printed because M3's
renderer is the thing that will make it non-zero, and a line that appears then is a line nobody
checks.

**The cooked scene is spawned, not streamed.** `EntityTemplate::spawn_many` copies into chunks that
are already partly full — one memcpy per column per row, about 11 ns an entity. The pure bulk copy
the storage argument is priced on is *cell activation*, which copies into fresh chunks and is M6's.
The spike measured both and they are not the same number; this sample exercises the one that exists.

**`--instances`, `--batteries` and `--turrets` change the hash**, because they change the world. The
figures in this file and the strings `tests/smoke/test_headless_sim.cpp` looks for are the defaults'.

**Windows and macOS are unverified.** The sample adds no platform-conditional code at all, but it has
only been run on Linux.

**Governed by**: `delivery-roadmap` (milestone artefacts), `ecs-core`, `scene-graph-and-nodes`,
`serialization-and-prefabs`, `engine-architecture` (the fixed-tick loop),
`simulation-and-determinism` (the commit boundary, seeded streams, hierarchical state hashing).
