# `src/scene/serialization/` — layer 4

Layer 4, target `cy::scene-serialization`, headers `<cy/scene/serialization/*.h>`, namespace
`cy::scene::serialization`. Section 3.2 of `openspec/changes/implement-m2-world/tasks.md`, governed
by `serialization-and-prefabs`.

Prefabs, scenes and worlds as authoring assets, and the cook that turns them into runtime data.
Nesting, variants and overrides resolve at cook time into immutable entity templates; a shipping
build carries no prefab link.

## The map

| Header | What it owns |
|---|---|
| `asset.h` | `AssetKind`, `LocalId`, `ParameterId`, `CookMode`, `MotionKind`, `FlattenPolicy` |
| `overrides.h` | The six override operations, override conflicts and their resolutions, provenance |
| `document.h` | `Document` — one authoring file: entities, instances, parameters, a variant base |
| `format.h` | The two forms of a document: canonical text, and the tagged binary |
| `library.h` | The asset graph: dependency order, cycle rejection as a chain, variant depth |
| `resolve.h` | Resolution into one concrete graph, with provenance; and the structural diff |
| `cook.h` | The six-step cook, hierarchy flattening, archetype blocks and reference sites |
| `spawn.h` | `EntityTemplate`, batch spawning into a `World`, and live prefab update |

## Six decisions worth knowing before changing anything here

**1. An override addresses three identifiers and nothing else.** The prefab-local entity id, the
component `TypeId`, the `FieldId`. Never a name path. This is the first data in the engine whose
correctness depends on an identifier having been assigned once and never reused, which is what M1's
manifest and its tombstones exist for — and `tests/test_identity.cpp` is where that stops being a
claim: a field is renamed, the override still applies; a field changes *identity*, and the migration
chain moves the override with the data.

**2. An override is never dropped.** One whose target has gone becomes a `ConflictKind`, retained on
the authoring data, reported by `resolve()`, and offered three resolutions — discard, retarget,
restore. Nothing in this module erases one on its own, and the conflict survives a save and a load.

**3. A placement's mapping is what makes a reference into an instance stable.** When a prefab is
placed, every entity it will contribute is given a local id **in the containing document**, once, and
the mapping records it. A reference into an instance is then an ordinary local id rather than a path.
An entity with no mapping entry gets a deterministic id at resolve time and **nothing may reference
one** — `populate_mapping()` is what an editor calls so that never happens.

**4. Parameters are applied before overrides, at each level.** Setting a field directly is the more
specific act than setting a parameter that happens to reach it, so an explicit override wins. That is
also what keeps "internals stay private" true: a prefab may re-bind a parameter to a different field
without changing what any instance's explicit overrides do.

**5. Flattening is a walk to the root, not a look at one edge.** An edge is needed when the child
moves *or when anything above it does*. The M2 spike found this: a static muzzle bolted to a static
barrel under a *rotating yaw* still needs its relationship, and a per-edge test flattens it out from
under the yaw with a failure visible only in motion. `test_cook.cpp` has that exact case.

**6. A cooked reference slot holds `template index + 1`.** The bias is what makes a null reference
and a reference to the first entity distinguishable in a slot that is zero-initialised. The reference
*sites* — column and byte offset — are emitted beside each block, so the spawn's fixup is a walk over
known columns rather than a reflection lookup per row; the spike measured the alternative at 4.7–5.2x.

## Where this is thinner than the specification, stated plainly

* **The reference fixup is not the strided pass the spike measured.** `ecs::World` exposes a row
  through `get_mut`, which is a table lookup and a binary search over the archetype's columns, and
  does not expose a column span for a run of rows. So a spawn pays one lookup per *referencing row*
  where the spike paid one per column. `EntityTemplate::fix_up_references` is the only function that
  would change when the ECS grows that accessor. **This is a request against `src/ecs/`.**
* **Apply-and-extract is not implemented.** `serialization-and-prefabs`' "Apply and extract"
  requirement — pushing an instance's overrides back onto its prefab, and lifting a subtree into a
  new prefab asset — is an editor operation over the data model. The data model supports it (`diff`,
  `Override::clone_into`, the mapping) and the operations themselves are not written. **M5.**
* **Packed scene instances keep their hierarchy and nothing more.** `CookMode::Packed` marks the
  spliced roots `FlattenPolicy::Keep`, so a packed composition stays a unit. Streaming it as one, and
  owning it as one, is `world-partition-and-streaming`'s at **M6**.
* **The transform is one 40-byte field.** M1's reflection has no vector field kind, so
  `TransformBinding` names a component and one opaque field holding a `cy::Transform`. When the
  generator learns about vectors this becomes three field descriptors and nothing else changes.

## The dependency list, and why it is not `cy::scene`

The scaffold declared this module against `cy::scene`, and it does not use it: cooking reads
authoring documents and writes archetype blocks, and a `Node` — a handle onto an entity, not a thing
a file contains — appears nowhere in that path. What it does use is `cy::ecs`, directly and
unavoidably: `World::instantiate`, `ArchetypeBlock` and `Entity` are what a cooked template is
spawned through. The module is still at layer `scene`, so nothing about the layer check changes.
