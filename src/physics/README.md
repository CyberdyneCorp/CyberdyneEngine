# `src/physics/` — layer 4: the physics ECS bridge

The components `physics` specifies, registered in a world; the bodies created from them; the stepper
driven in the `Physics` stage; and `cy::scene::LocalTransform` written back.

**Governed by**: `physics`. Section 4 of the M8.a tasks.

## Why this directory exists, in the words of the file that asked for it

`src/servers/physics/include/cy/servers/physics/components.h`, since M4:

> A component is two things: a layout, and a registration in a world. The LAYOUT is what a scene
> file stores … and it belongs beside the server whose vocabulary it is written in. The REGISTRATION
> belongs with whoever owns the world. `cy::scene::LocalTransform` … is layer 4, and this directory
> is layer 2, so the bridge that reads `RigidBody`, creates a body and writes `LocalTransform` back
> cannot live here. **It is `src/physics/` at layer 4, mirroring `src/rendering/scene/`, and it does
> not exist yet.**

M4's gate flagged the absence; M5.5, M6 and M7 did not close it; M8.a's proposal names it as one of
the four things it verified missing rather than assumed. This is that module.

## What is here

| | |
|---|---|
| `components.h` | `PhysicsComponents`: the eight component ids in one world, and `register_all` |
| `bridge.h` | `PhysicsBridge`: create, sweep, step, publish, tear down — and `install` into the `Physics` stage |
| `state_schema.h` | The eight components declared to the state hash, so a divergence in a body is a divergence in the hash |

## Read `samples/04-character`'s host before you change `bridge.cpp`

Task 4.2, and design.md §3: that sample is this module written out longhand, which is why its
`game.cpp` is larger than its 667 lines of Swift. It was read, and `bridge.h`'s header names the four
things it does and the four ways this differs — a component vocabulary instead of a Swift contract, a
`PhysicsStepper` instead of a bare `step`, a removal sweep it never needed, and a publication into
`LocalTransform` instead of into a report. **The behaviour was settled; only its home was not.**

## Four decisions worth knowing

**It does not own the backend.** It is handed a `PhysicsServer&` and a `WorldHandle` and creates
neither. `physics` requires that swapping the backend change no gameplay, and a layer-4 module that
named one would be the layer that decides which. `-D CY_PHYSICS=OFF` therefore changes nothing about
this module: it is built, tested and green against the reference backend.

**One entity's refusal is not the world's.** A `RigidBody` with no `Collider` has no volume,
therefore no derived mass, and `cy::physics::validate` refuses it — correctly. An editor that stopped
simulating everything else because of one such entity would be unusable, so the refusal is counted in
`BridgeStatistics::bodies_refused`, named in `last_error()`, and the rest of the world simulates.

**The publication marks the node dirty as well as writing it.** Writing `LocalTransform` is half of
publishing a placement; the other half is telling the scene layer that an authored transform changed
so `propagate()` derives `WorldTransform` from it. That is why the constructor takes a `SceneTree`
rather than a `SceneComponents`, which is what the renderer's equivalent takes.

**The authored scale survives a step.** A solver has no opinion about scale — a shape carries its own
dimensions — so `publish` keeps the component's scale and writes only the placement. A bridge that
wrote the body's transform whole would reset every scaled object in the world on the first tick of
play, which is exactly the residue task 5.2 is about.

## Teardown is a test, not a comment

`tests/test_teardown.cpp`, and M8.a task 4.4. M5.5's gate found Jolt's job bridge destroying its
free list underneath a worker still releasing a job — one run in forty, as a fault with no physics
call on the stack. The backend holds the regression for a bare world; this holds it for the shape
**play mode** produces: an ECS world, a scene tree, a bridge with a body per entity and a stepper
with a record per body, created and destroyed sixty-four times with four spinner threads holding the
cores and physics tasks still draining.

Four orderings are covered, and the third is the one nobody writes by accident: the physics **world**
destroyed before the bridge that holds its bodies, which is what a runtime dying under an editor
produces.

## What is not here yet

* **Constraints.** `Joint` is registered and counted in `BridgeStatistics::joints_deferred` and
  nothing is created, because neither backend maps constraints — both report
  `Capabilities::constraints == false`. A bridge that tried would fail at every world with a
  diagnostic about a component the author was entitled to add.
* **The character controller.** `CharacterBody` is registered and counted the same way.
  `cy::physics::CharacterController` holds a pointer to the server and is not chunk-storable; who
  owns the controller object is a gameplay question this module cannot answer.
* **Compound colliders.** One `Collider` and one `Trigger` per entity, because a component is one per
  type per entity. Several colliders on one body needs a buffer component, and that is an authoring
  change as much as a bridge one.
* **Reflected components.** The eight are registered with `register_builtin`, by name, with no
  `reflect::TypeInfo` and no manifest identifier — which is why `state_schema.h` exists, and why
  `cy::scene::serialization::resolve_against` carries a `RigidBody` in a `.cyworld` by name rather
  than resolving it. `src/gameplay/play/src/session.cpp` reads them by name for that reason and says
  so. Assigning them identifiers is the one line of follow-up this module owes.
