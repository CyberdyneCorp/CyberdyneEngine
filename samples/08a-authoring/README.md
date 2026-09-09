# `samples/08a-authoring` — a scene a person builds by hand, and a game they can press play on

> **M8.a's closing artefact.** Section 6 of `openspec/changes/implement-m8a-authorable/tasks.md`.
>
> ```
> just run-authoring                                   # run it
> just run-authoring --no-window                       # ... without the photograph
> just run-authoring --physics reference               # ... the negative control; it must fail
> just run-authoring --shot docs/design/images/authoring-m8a.png
> just run-sample authoring --world <path.cyworld>     # the measuring half alone
> ```
>
> CTest entry: `smoke.authoring`. Needs no display; the photograph is the one act that does, and it
> reports itself NOT EVALUATED where there is none.

![The editor's window, showing the world this session authored: the engine's own rendered frame in
the viewport, two entities in the outliner, one placed three metres above the
other](../../docs/design/images/authoring-m8a.png)

## The sentence this milestone exists for

> *"add Box, Cylinder, Sphere in a scene, add physics, place the object at different positions and
> play the scene."*

Eight milestones in, none of it was possible. This artefact is that sentence, run end to end:

```
create a sphere and a box in an empty world
place the sphere above the box with the gizmo
add a rigid body to each, in the inspector
press play — the sphere falls and lands on the box
stop — the world is exactly as it was authored
undo back to an empty world, exactly
```

**It is one session, not six checks.** Every step happens in one editor process, over one control
socket, against one engine runtime, in the order above. A fixture cannot satisfy any of them,
because each step's evidence is the state the previous one left — which is the point: primitive
creation, the gizmo's manipulation, the physics ECS bridge, play mode and the transaction system are
exercised *together*.

## Three processes, and what each is for

| | |
|---|---|
| `cy_editor_window_runtime` | **the engine.** It loads the same `.cyworld` the editor opens, applies the editor's transactions as they commit, and hosts the play session — `cy::gameplay::PlaySession` over `cy::physics::PhysicsBridge` on Jolt. Its own closing report is the second witness to every claim below. It needs a Vulkan device that can export a dma-buf; where there is none the target does not exist and the acts that need it are NOT EVALUATED. |
| `cyberdyne-editor --mcp` | the editor, attached to that runtime with `--host`, driven through its own command registry over the Model Context Protocol. No window, no graphics device. |
| `cy_sample_authoring` | **the measurement.** See below. No display, no device: this is the half a hosted runner can judge. |

## Why there is a second program

The runtime's report counts sessions, ticks and bodies. It does not count metres, and no message the
editor can ask for carries the simulated placement of a node — the only place it appears is inside
the runtime's own copy of the authored world. So *"the sphere falls and lands on the box"* would be
a claim with no number behind it.

`cy_sample_authoring` is where the number comes from, and **it plays the same authored world**: the
empty `.cyworld` this project ships, plus the editor's own committed transactions, replayed with
`cy::scene::serialization::apply_transaction` out of the `.cyjournal` the session wrote.
`editor-documents-and-transactions` requires that *"the journal SHALL be the same operation stream
used by diff, live editing, and any future collaboration, rather than a separate representation"* —
a second consumer replaying it is what that sentence is for, and it is the same decoder the runtime
calls on every live edit.

It reports where the sphere started, where it came to rest, whether that is the height the two
colliders imply, and whether `stop()` put the file back byte for byte — comparing the bytes itself
rather than reading `PlayReport::restored_exactly` and believing it. Then it runs the whole session
a second time over the same world, because a session that left a body, a velocity or a nudged
placement behind would make the second fall end somewhere else.

## What one run measures

| Figure | On the machine this was built on |
|---|---|
| the height the sphere was authored at | 3000 mm |
| the contact height the two colliders imply | 750 mm — the static box's top at 250 mm plus the sphere's 500 mm radius |
| **median resting height of the sphere** | **729.9 mm** over six sessions, every one identical to the last micrometre |
| median ticks to come to rest | 55, at 60 Hz |
| the lowest the sphere reached | 681.6 mm |

The sphere rests **20 mm inside** the contact height, which is Jolt's own penetration allowance and
not an error in the bridge. The artefact's threshold is 50 mm, and a sphere that went *through* the
box rests four hundred metres below it, which is what the negative control shows.

## The negative control, and why it is a real backend

```
just run-authoring --physics reference        # records a gap and returns 1
```

`cy::physics::reference` declares `Capabilities::contact_resolution = false`: it integrates motion
and resolves nothing. So the sphere falls through the box, the run records

```
GAP   the sphere lands on the box — it came to rest at -415518.6 mm and the two colliders put
      contact at 750.0 mm, which is 416269 mm out — far enough that it went through
```

and returns **1**. That is task 6.7's *"a recorded gap exits non-zero"* demonstrated rather than
asserted, and it is a switch that changes the subject rather than one that fakes a failure.

## The surface this artefact drives, and the three refusals it performs

`cyberdyne-editor --script` is the obvious way to drive a session and **cannot drive this one**.
`cy_editor_app::run_script` makes every argument a `Value::Text` and the registry validates argument
kinds, so

```
scene.translate amount=0,3,0
    the parameter "amount" is declared vec3 and a text was supplied
```

and a world in which nothing can be *placed* is not this milestone's world. It is not only `vec3`:
a `float`, an `int` and a `bool` are refused the same way, so `scene.add-body mass=2` is refused
too. The agent surface parses a typed argument out of the identical text
(`cy_editor_agent::tool::lanes`), which is why this artefact drives the editor as an agent — and the
run *performs* that refusal on every pass, so the day `run_script` reuses the agent's parser the
note goes stale loudly rather than quietly.

The agent surface costs two more refusals, and the run performs both, because each is the scope
working rather than a defect:

* **`scene.create-primitive` is refused.** It writes its `.cyprim` source to `assets/primitives/`
  and the `author` scope grants `game/` and nothing else. The artefact asks for the one-call command
  first, records the refusal with the scope as its reason, and then does the two halves the scope
  does permit: `asset.write-primitive` writes the same source under `game/`, and `asset.import` puts
  it in the world. **The entity is the same entity** —
  `cy_editor_services::primitives::create_mesh_instance` is the one constructor of a mesh instance
  and both callers use it, which is task 2.3 guaranteed rather than tested for. The day a scope
  exists that grants an assets directory, the artefact takes the one-call path with no change here.
* **`file.save` is refused**, because it is an irreversible mutation and the `author` scope grants
  read and reversible mutation. The session therefore never writes the world, which costs nothing:
  the engine's copy is the runtime's, and the measurement's copy is rebuilt from the journal.
  `cy_sample_authoring --write` is what finally puts it on disk, through the engine's own writer,
  and that file is what the photograph is of.

## What the picture does not show

**The two entities are drawn as unit boxes.** `samples/05b-editor-window/runtime` presents a world
through M3's fixed scene slots, and the `MeshRenderer` reference a primitive writes reaches no
renderer yet — `cy::render::MeshRenderer` is a declared name in the node-template catalogue with no
reflected type behind it. So the sphere above the box is a box above a box. What is real in the
picture is the placement, the hierarchy, the transport and the fact that those pixels came out of
another process's GPU allocation: 43% of the viewport is lit against 0% of the editor's charcoal
chrome beside it.

**The outliner labels both rows `Transform`.** A node has no name in `cy_editor_documents` — the
third field of a `.cyworld`'s `node` line is its *layer*, not a name — so
`cy_editor_viewmodels::hierarchy::label_of` falls back to the first component that has fields.
`scene.create-primitive`'s own description says its `name` argument "names the source asset, the
generated mesh sub-asset **and the node**"; it names the first two. Naming a node is a concept the
document model does not have, and it is the thing this artefact most wants and cannot have.

**The keyboard is not what took the picture.** `play.enter` is bound to `F5`, so pressing play in
the window would be one synthesised key — but on the X session this was built on, XTEST key events
do not reach this editor's window at all: `Ctrl+P` opened no palette and `F5` reached no runtime,
while every process involved was healthy. `samples/05b-editor-window` succeeds with keys on the same
machine and carries about two hundred lines of focus, wake and retry machinery to do it; rather than
copy that here, this act takes one photograph and says so. Pressing play is proven in the session
above, over the editor's own control socket, and how far the sphere fell is measured by
`cy_sample_authoring`.

## The project this artefact authors into

`project/` is copied into the build tree on every run rather than used in place, because the run
writes `.cyprim` sources, cooked assets and a journal, and a source tree that accumulated them would
make the second run a different run from the first.

`project/worlds/authored.cyworld` is **an empty world with a schema**: twelve type declarations and
no nodes. The schema is there because a transaction addresses types and fields by the numbers the
*file* declares — `apply_transaction`'s header explains why, and there is no `DeclareType` operation
for a schema to cross the wire on. A runtime whose world did not already declare `RigidBody` would
apply the editor's `AddComponent` and build **zero bodies**, which is exactly what the first version
of this artefact did.

Those twelve declarations were **written by the editor itself** — a scripted session created a box
and a sphere, added a body to each, saved, and the nodes were then removed, leaving the type section
the editor's own `worldfile::write_world` produced. So the names and numbers in that file are not a
hand-made guess at what the editor writes; they are what it writes.
