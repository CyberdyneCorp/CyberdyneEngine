# Design: CyberWorld and the authoring model

## 1. Two things called "world" is one too many

`ecs-core` already owns `World`: the runtime container of entities, archetypes, and schedules. The
system this change adds is also, naturally, called the world. Left alone, that ambiguity would
reach the public API and never be recoverable.

The rule, stated once and enforced by naming:

- **`World`** — the ECS runtime container. Unchanged.
- **The persistent world** — the spatial and persistence layer *above* it, addressed as
  `WorldAsset`, `WorldPartition`, `WorldStreaming`, `WorldLayers`. Never as `World`.

The relationship is one-directional: the persistent world knows what exists, where it belongs,
whether it should exist now, and where its persisted state lives. The ECS world knows only what is
currently instantiated. The persistent world publishes into the ECS world; the ECS world never
calls back into it during activation.

## 2. Four things that are not the same thing

The single most valuable distinction in this change, and the one that most engines blur:

| Axis | Question | Owner |
|---|---|---|
| **Cell residency** | Are this region's bytes and resources in memory? | World streaming |
| **Cell activation** | Do its entities participate in simulation? | World streaming |
| **Asset residency** | Are its textures, meshes, and audio resident, and at what detail? | Asset streaming |
| **Simulation detail** | How much thinking, animation, and physics does it get? | AI, animation, physics LOD |

Collapsing them is what produces the classic hitch: crossing a boundary triggers *load everything
now*, because residency and activation were the same event. Kept separate, approach becomes a
gradient — metadata at 2 km, coarse geometry resident at 1 km, collision prefetched at 500 m,
entities instantiated at 300 m, full detail at 100 m — with each transition cheap because the
expensive part already happened.

## 3. Coordinates: cell-relative, not global doubles

`core-math` says large-world support comes from camera-relative rendering "rather than by making
the whole engine double precision". That decision stands, and this change makes it stronger rather
than reversing it.

The authoritative persistent form of a position is **cell-relative**:

```
WorldLocation = { CellCoord cell; float3 local; }
```

This is better than a global `double3` for reasons beyond precision: it is smaller on the wire, it
is stable under world origin changes, it is directly indexable for spatial queries, and it aligns
with the streaming unit for free. A `double3` accessor exists for tooling, geodetic work, and
interchange — not as the runtime representation.

Precision then falls out: local coordinates are bounded by cell size, so 32-bit floats are exact
enough everywhere, at any distance from the world origin, without rebasing tricks in gameplay code.

## 4. The authoring partition is not the runtime partition

Designers organise a world for collaboration and comprehension. The runtime needs it organised for
streaming and memory. These are different objectives and forcing one structure to serve both makes
the project hostage to a performance decision.

So authoring source is chunked for source control, the cooker partitions independently, and
**changing runtime cell size does not require designers to reorganise anything**. It is a cook
setting.

## 5. One file per authoring unit, not one file per entity

Externalising each actor into its own file solves real source-control contention. Applied to an
ECS world it does not survive the arithmetic: five million entities is five million files, which no
filesystem or version control system handles gracefully.

The unit is therefore the **authoring unit** — a prefab, a scene, or an authoring chunk of a world
region holding hundreds to a few thousand entities. Because entity identity is stable and
independent of file placement, an entity can move between chunks without changing identity, so
chunk boundaries can be rebalanced without breaking references.

## 6. Cells cook to archetype blocks, and activation is a copy

A cooked cell is not a serialised object graph. It is column-major component data already grouped
by archetype:

```
archetype {Transform, MeshRenderer, Collider}
  ids        [.....]
  Transform  [.....]
  MeshRenderer [...]
  Collider   [.....]
```

Activation becomes: allocate chunks, decompress, bulk copy, fix up references. Not one hundred
thousand object constructions. This is the direct dividend of having built an archetype ECS, and it
is what makes a large cell activation affordable at all.

## 7. Activation is transactional *and* incremental — which is a tension, resolved

Two requirements pull against each other. Systems must never observe a half-activated cell — a
building whose collision exists but whose entities do not is a bug factory. But a cell with a
hundred thousand entities cannot be activated in one frame without a visible stall.

The resolution is that preparation happens in **private staging** across as many frames as it
needs, and **publication is atomic**. Chunks are allocated, component blocks decoded, physics
batches built, and GPU scene data uploaded without any of it being visible to systems; then the
cell is published in a single step. The world HLOD proxy remains visible throughout, so the player
sees continuity rather than pop-in.

Systems observe two states, never a third.

## 8. Cross-cell references never dereference

A persistent reference is a `PersistentEntityID`, resolved through a registry, and it is valid
whether or not its target is loaded. It is never a pointer and never a runtime entity id.

Each reference declares a **policy**, and the policy is what the cooker can reason about:

| Policy | Meaning | Cook implication |
|---|---|---|
| `Soft` | May be unresolved; caller handles null | None |
| `LoadOnDemand` | Resolution triggers a streaming request | Latency, not a dependency |
| `RequireLoaded` | Target must be resident whenever the holder is | **A hard cell dependency** |
| `FollowOwner` | Lives with its owner, not with a cell | None |

`RequireLoaded` is the dangerous one, and it is dangerous transitively: cell A requires B, B
requires C, and walking into A pulls half the world. So the cooker computes the transitive closure
and **reports it as a number** — "activating cell A pulls 312 cells, 4.8 GB" — with the chain that
caused it. A dependency explosion should be a build report, not a discovery on console.

## 9. Authored world plus delta equals current world

Cooked cells are immutable and content-addressed. Runtime changes never rewrite them.

```
authored cells + persistent overlay = current world
```

The overlay records entity creation and removal, component changes, layer states, and world state.
One mechanism then serves save games, dedicated server persistence, replays, and the editor's
play-mode changes — which is why it is worth defining as a first-class concept rather than letting
each of those grow its own.

## 10. Layers are orthogonal to space

A cell contains entities from several **layers** — base environment, a destroyed-city scenario, a
seasonal variant, debug content — and layer activation is independent of cell streaming.

This is what makes world state changes cheap. Turning a city into its destroyed version is one
layer activation, not twenty thousand property changes, and over the network it is one identifier
and one state rather than twenty thousand messages. Layers carry stable identifiers rather than
names, so renaming a layer does not rewrite every entity that belongs to it.

## 11. World HLOD and virtual geometry solve different problems

Both reduce distant cost and they are not substitutes:

- **Virtual geometry** reduces *triangle* detail within an object, continuously.
- **World HLOD** replaces *many objects* with one aggregate — two thousand buildings become a
  district proxy — so distant regions need neither entities nor their assets.

A district proxy is itself virtual geometry. One works inside the object; the other decides whether
the object exists at all.

## 12. Prefabs are authoring; entity templates are runtime

A prefab is an authoring graph with nesting, variants, overrides, and provenance. Resolving all of
that at spawn time would be absurd, so cooking compiles it to an immutable **entity template**:
archetype blocks plus relationships, ready for batch instantiation.

Shipping builds keep no prefab link. Development builds keep provenance, which is what makes live
prefab editing possible — and live editing forces a question the specification must answer: when a
designer changes a prefab while the game is running and an instance's health is at 40 %, what
happens?

The answer is **field classification**, declared through reflection:

| Class | Live update behaviour |
|---|---|
| `Authoring` | Updated from the new template |
| `RuntimeState` | Preserved |
| `PersistentState` | Preserved and saved |
| `Derived` | Recomputed |

`Health.max` is authoring; `Health.current` is runtime state. Without this the choice is between
resetting gameplay state on every prefab edit or never updating anything, and both are bad.

## 13. Exposed parameters are a prefab's public interface

Arbitrary component overrides make every internal detail of a prefab part of its contract, so any
refactor breaks instances.

A prefab may instead **expose parameters** — height, light colour, team, material — each of which
may drive several internal fields. A scene sets parameters; the prefab's internals stay private and
refactorable. Ad-hoc overrides remain available, because they are genuinely needed, but the
parameter surface is the recommended path and the one that survives change.

This is the part of the authoring model that is a real improvement on the engines being borrowed
from, rather than a reimplementation of them.

## 14. Override conflicts are a state, not a warning

`serialization-and-prefabs` currently says that when a prefab deletes a component an instance had
overridden, the stale override is dropped on load with a development-build warning.

That is wrong in a way worth correcting: in a shipping build it silently discards designer intent,
and in the editor a warning in a log is not where the person who can fix it is looking. Overrides
that no longer have a target become an explicit **conflict** — surfaced in the inspector, reported
by validation, and resolvable by discarding, retargeting, or restoring the deleted structure.
Nothing is dropped without someone deciding.

## 15. Macro representation: the world decides existence, AI decides thinking

A distant army should not require ten thousand entities, but it should still exist.

The world defines **representation tiers** — full entities, aggregate, statistical — and owns
promotion and demotion between them as streaming state changes. `ai-system` already specifies AI
LOD down to a statistical population model; this is the mechanism by which that model becomes
entities and back again. The division: the world owns *whether content exists in a form*, AI owns
*how much it thinks in that form*.

## 16. Build order

| Milestone | Contents |
|---|---|
| M1 | World coordinates, persistent identity, world asset, cell index |
| M2 | Cell cooking to archetype bundles; load, unload, activate |
| M3 | Streaming sources, priority planner, asset integration, budgets |
| M4 | Prefab assets, nesting, entity template compiler, batch spawn |
| M5 | Variants, overrides by stable identifier, exposed parameters, diff and conflicts |
| M6 | Scene assets, scene instances, embedded and packed cooking |
| M7 | World layers and scenario states |
| M8 | Cross-cell references, persistence overlay, saves |
| M9 | World HLOD, predictive streaming, streaming channels |
| M10 | Network integration, server profiles, macro representation |
| M11 | Authoring chunks, collaboration, streaming debugger, profiler |

**M2 is the milestone that matters.** Cooking cells into archetype bundles and activating them by
bulk copy is the thing that has to be true for everything else to be worth building; if activation
is slow, no amount of scheduling hides it.

## 17. Gaps

- **Virtual textures** — still unspecified, and now the last major streaming consumer without an
  owner.
- **Virtual shadow maps** — still unspecified.
- **Procedural and runtime-generated worlds** — the partitioner interface admits them, but
  authoring, cooking, and identity for content that does not exist at cook time are not specified.
