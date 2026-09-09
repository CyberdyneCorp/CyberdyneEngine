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

## The authoring schema, and how an editor in another process sees the engine's components

`authoring_schema.h` is M6 task 2.4. It reads `cy::reflect::TypeRegistry` and writes a deterministic
text manifest — `cyschema 1` — that the editor reads with no engine in the process at all. Before it,
`DocumentService::open` produced a name and an empty schema, so nothing in a document was described:
nothing was selectable, no `Transform` bound, and a gizmo drag committed nothing.

Two rules do the work, and both are in the header's own note:

* **Consecutive scalar lanes with a common dotted prefix are grouped.** Reflection describes
  `LocalTransform` as ten `f32` fields named `value.rotation.x` … `value.scale.z`; an inspector
  generated from that is ten spin boxes and a gizmo has nothing to bind to. The group takes the
  identifier of its **first** lane, so an override or a history entry addressing it still names the
  same bytes.
* **One alias, declared rather than derived.** The ECS component is `cy::scene::LocalTransform` and
  the authoring concept a designer moves is `Transform`. Deriving that — stripping a `Local` prefix,
  say — would be a rule that silently renames the next component somebody adds.

`samples/05b-editor-window/project/types.cytypes` is the committed manifest, and
`cy_test_unit_scene_serialization` regenerates it and compares byte for byte. That is what stops a
project's schema drifting away from the engine that has to load its worlds; it is the same shape
`just generate-check` uses for the reflection headers.

## `.cyworld`, read and written here — M8.a tasks 1.1 and 1.2

M7's gate recorded the gap this closes: *"`.cyworld` is a third authoring format that nothing under
`src/` or `tools/` reads, beside `cydoc` and `CookedCell`"*, and the editor's document and the
runtime's scene were *"associated in first-seen order by a file whose own header calls itself a
stand-in"*.

`worldfile.h` is the engine's reader and writer for that format, and `world_transaction.h` applies
the editor's own operation stream to what it produced. Three properties make the two sides one
world rather than two that agree:

* **Identity is derived on both sides, never transmitted.** `DocumentId` is an FNV-1a-128 of the
  document's asset path and a `NodeId` is the same hash of that and the node's ordinal; a node
  written at file position `p` is ordinal `p + 1`, because `cy_editor_services::worldfile::load`
  creates them in file order. `editor_document_identity` and `editor_node_identity` compute what
  the editor computed, and `test_worldfile.cpp` and
  `cy_editor_services::worldfile::tests::the_engine_derives_the_same_identities` pin the same four
  numbers from opposite sides.
* **The file's type numbers are the transaction's.** A `.cyworld`'s `type` section is written out of
  the document's own `DocumentSchema`, so a `SetField` naming component 3 field 5 names exactly the
  field the file declared as `scale`. Nothing infers a field's meaning from the shape of its value,
  which is what `samples/05b-editor-window/runtime/session.h` had to do and said it should not have
  to.
* **A round trip is byte-identical**, including a type this build has never heard of, which is
  `serialization-and-prefabs`' requirement that an editor without a plugin not silently strip that
  plugin's data.

**What remains two formats.** `.cyworld` and this module's `cydoc` are still two authoring text
grammars. `cydoc` carries what a world file does not — prefab instances with their overrides and
parameters, variants, motion classification, flattening policy — so folding them together is a
format decision rather than a reader, and it is not this milestone's. What is no longer true is that
nothing under `src/` reads a world.

## The dependency list, and why it is not `cy::scene`

The scaffold declared this module against `cy::scene`, and it does not use it: cooking reads
authoring documents and writes archetype blocks, and a `Node` — a handle onto an entity, not a thing
a file contains — appears nowhere in that path. What it does use is `cy::ecs`, directly and
unavoidably: `World::instantiate`, `ArchetypeBlock` and `Entity` are what a cooked template is
spawned through. The module is still at layer `scene`, so nothing about the layer check changes.
