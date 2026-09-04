# `src/scene/` — layer 4

The authoring and scripting façade. A `Node` is a named, hierarchical handle onto an entity, giving
designers and script code an object-oriented view of the same world the ECS stores.

**Governed by**: `scene-graph-and-nodes` (this directory) and `serialization-and-prefabs`
(`serialization/`). Arrived at M2, tasks 3.1.1–3.1.10.

## The one rule

**A node never owns data.** `Node` is a `SceneTree*` and an `Entity` — sixteen bytes, trivially
copyable, and asserted to be exactly that in `node.h`. Every property it exposes reads or writes the
underlying component through the ECS. There is no shadow copy, no dirty-flag pair and no
"sync the node to the entity" step, because the moment one exists the two representations can
disagree and every bug after that is a debugging session about which one was right (design.md §3).

Two consequences worth stating, because they are what the rule buys:

* Every mutator on `Node` is `const`. Writing through a handle changes the world, not the handle —
  the same relationship a `T* const` has with its pointee. A non-const mutator would suggest the
  node holds the value.
* Nothing invalidates a node. `valid()` asks the entity table, so a node whose entity a system
  destroyed is invalid the instant the destroy lands, with no invalidation pass anywhere.

## The authored/derived split

The module is organised around one line, drawn three times:

| authored | derived | written by |
|---|---|---|
| `LocalTransform` | `WorldTransform` | transform propagation |
| `NodeFlags` (visible, enabled) | the `Hidden` and `Disabled` tags | flag propagation |
| `ChildOrder` | the ECS `Children` buffer | `World::set_parent` |

A derived component is not a cache: it is a different value, computed from the authored one *and the
parent chain*, by exactly one system, at exactly one point in the frame. A stale cache may be read
and be merely wrong; a stale derived component is a coherence violation, and `coherence.h` is the
test that says so.

Effective visibility and enablement **are** the `Hidden`/`Disabled` tags rather than bools beside
them, because `scene-graph-and-nodes` requires a disabled subtree to be excluded from gameplay
queries — which a bool cannot do, since a query would still visit every row to test it.

## What is here

| header | what it is |
|---|---|
| `components.h` | the components a node is made of, and their per-world ids |
| `node.h` | the handle: naming, hierarchy, paths, transforms, flags, components, groups |
| `tree.h` | `SceneTree` — one world's node layer, the registries, and the frame pump |
| `node_template.h` | node types as data, and the shipped catalogue |
| `propagation.h` | the one depth-first walk that computes both derived values |
| `behaviour.h` | behaviours, the batching decision, the report, and the dispatch systems |
| `group.h` | groups as tag components, and broadcast |
| `scene.h` | a scene description, its identity, and its status |
| `coherence.h` | the five invariants, as a report rather than as an assertion |

## Three things that are thinner than they look

**The shipped template catalogue is declared, not live.** `scene-graph-and-nodes` lists the node
types the engine ships — mesh renderer, camera, lights, bodies, colliders, UI host, sprite,
tilemap. All twenty-three are declared, as data, in `node_template.cpp`. Twenty-two of them name
components that belong to milestones that have not happened (no renderer until M3, no physics or
audio until M4 and M8, no UI until M7), and `NodeTemplateRegistry::status()` reports them as
declared-but-not-instantiable rather than pretending. Spatial grouping is live, because every
component it names belongs to this module.

**Behaviour batching is decided from the declaration, not by a compiler.** `decide_dispatch()`
implements the rule `scene-graph-and-nodes` states — arbitrary script per entity per tick,
per-instance state, or access outside the declaration each force per-instance dispatch — and the
lowered form is supplied by the author as `fixed_update_batch`/`update_batch`. The report, the
fallback, the system-shaped dispatch and the reasons are all real now; the *lowering* is hand-written
until the script toolchain at M4/M5, which will fill those two pointers in and set
`invokes_script` from analysis rather than from the author's word.

**Propagation reads components through `World::get_mut`**, which the ECS documents as a table lookup
and a binary search. That is right for the changed set, which is what a propagation visits — a
quiet frame visits the roots and stops. It is the first thing to revisit if a frame ever has to
propagate a whole large hierarchy; the shape to move to is depth buckets iterated as chunks.

## Where a system fits in

`SceneTree::install_systems(schedule)` registers, and after it nothing in the frame calls a
behaviour or writes a derived value except a registered system:

| stage | system |
|---|---|
| `Simulation` | `scene.behaviour.<name>.fixed` per batched behaviour, `scene.behaviour.dispatch.fixed` |
| `PostSimulation` | `scene.propagate_simulation` |
| `Frame` | `scene.behaviour.<name>.frame` per batched behaviour, `scene.behaviour.dispatch.frame`, `scene.propagate_render` |

`SceneTree::pump()` is the other half of the frame: it is called once, after the stage flush, and
dispatches the callbacks that are transitions of tree *shape* (`onEnterTree`, `onReady`,
`onExitTree`) and of effective enablement (`onEnable`, `onDisable`). Those cannot fire at the moment
the shape changes, because a subtree is attached one edge at a time and `onReady` is defined as
running after all children are ready.

## One engine-wide fix this module forced

`cy::Vec3 cy::reflect(Vec3, Vec3)` in `<cy/core/math/vec.h>` and the namespace `cy::reflect` in
`<cy/core/reflect/...>` are a function and a namespace of one name in one scope, which is an error in
**either** include order. Nothing noticed while no module needed geometry and reflection in one
translation unit; this one is the first that does, and every module after it — the renderer, physics,
animation — would have hit the same wall. The math function is now `reflect_vector`, with the reason
written at its declaration so the short spelling is not reintroduced. Its only callers were its own
two test assertions.

## Seams left for a later milestone

* **The scene components are registered by name, not by manifest identifier.** Exactly the seam
  `src/ecs/README.md` records for `Parent` and `Children`: the reflection generator's annotated
  header list lives in `src/core/reflect/CMakeLists.txt` and the identifiers in
  `identity/manifest.toml`, neither of which this module owns. Fabricating identifiers would be
  inventing the one kind of number `core-type-system` says must be assigned once and never guessed.
  Moving them across is a change to `components.cpp`'s two registration calls and to nothing that
  consumes them. **Their absence from the state hash did not wait for that**: M3's task 1.2 declares
  all twelve to `determinism::StateSchema` in `state_schema.h`, with each field's simulation class
  stated, so a divergence in a node's name, its parent, its sibling order or its visibility changes
  the hash — which none of them did at M2's close. `integration.state_hash_coverage` is the claim,
  run.
* **`InterpolatedTransform` is classified and the other eleven are not.** M3's task 1.3 adopted
  `determinism::Classified<>` on the component whose fields are unambiguously presentation state, so
  an authoritative system cannot read a render blend — the read does not compile. `components.h`
  names the next two (`WorldTransform` and `NodeState`, both `Derived`) and states the cost: every
  reader of a classified field has to carry a witness, which is a change to this module's bodies
  rather than to its data.
* **A group and a batched behaviour each consume one of the world's 256 component slots**, for the
  life of the world. That is what "groups SHALL be implemented as tag components" costs, and it is
  the right cost for the tens of named sets a game broadcasts to. A per-entity attribute with
  thousands of values is a component with a field, not a group.
* **`SceneRef` is a plain data component.** A shared component would group a scene's entities into
  their own chunks and make an unload a chunk walk instead of a filtered one — the better shape at
  M6, when a streaming cell is a scene. It costs a second structural operation per node at load
  time, so M2 does not pay it; the change is local to `instantiate` and `unload`.
