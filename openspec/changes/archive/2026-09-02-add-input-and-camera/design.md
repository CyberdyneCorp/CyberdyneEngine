# Design: CyberInput and CyberCamera

## 1. Three layers, and the boundary between each

```
platform devices → CyberInput → gameplay commands
```

The platform layer owns devices, normalisation, hot-plug, and **timestamps**. The input layer owns
users, contexts, bindings, processors, triggers, and actions. Gameplay owns commands. Each boundary
is a requirement rather than a convention, because both are routinely leaked across in practice: a
gameplay system that reads a key code, or an input layer that mutates a transform.

The existing platform requirement already provides an action map, and it is the wrong home for one:
an action model needs users, contexts, and triggers, none of which belong in a platform abstraction
whose job is to normalise operating-system events. It is superseded rather than extended.

## 2. Input users make local multiplayer structural

Most engines treat "the keyboard" and "the player" as the same thing and then retrofit local
multiplayer, which is why device hot-plug and split screen are perennially awkward.

An **input user** owns a set of devices, a context stack, and a rebinding profile. Two people on one
machine are two users; a disconnected controller is a user without a device, not a lost player. The
gameplay-level participant is unchanged by any of it, because participant identity and input user
are already separate concepts.

Device ownership is **explicit and policy-driven** — exclusive, shared, or split for keyboard and
mouse — rather than a global assumption that keyboard input belongs to player one.

## 3. The context stack, not a set of toggles

Enabling and disabling action maps globally is the usual model, and it fails in exactly the way you
would expect: a modal dialogue opens over an inventory over a vehicle, and someone has to remember
what to re-enable.

A **stack with priorities and consumption** handles it structurally. A context may consume an
action, override a binding, or augment a lower context, and it is pushed and popped by handle rather
than by remembering order. Gameplay features push their own contexts on activation, so entering a
vehicle installs vehicle bindings without a global switch.

## 4. Processors shape numbers; modifiers change meaning

Both are transformations and conflating them produces a soup where dead zone, sensitivity,
inversion, camera-relativity, and "only while aiming" are all the same kind of thing.

**Processors** are numerical and mostly stateless: dead zone, curve, scale, invert, clamp, smooth.
**Modifiers** are contextual: relative to camera, relative to the controlled entity, scaled by a
setting, active only in a state. Keeping the distinction means a processor chain can be evaluated
without knowing anything about the world, which is what makes it cheap and testable.

## 5. Mouse and stick are not the same signal

A mouse delta is displacement; a stick is a rate. Multiplying a mouse delta by delta time makes
look sensitivity frame-rate dependent — the single most common input defect in shipped games, and
one that is nearly invisible until someone plays at a different frame rate.

So a binding declares its **interpretation** — delta, absolute, or rate — and the processing chain
and camera controller integrate accordingly. Making this explicit in the binding metadata is what
prevents it being rediscovered per project.

## 6. Fixed-tick sampling preserves time

Input arrives at display rate; simulation runs at a fixed rate. Reading whatever value is current
when a tick begins loses presses, double-counts others, and makes replay unfaithful.

Events carry high-resolution timestamps, are accumulated between ticks, and are resolved into a
**command frame** per tick. A press and release inside one frame is still observed. This is the
mechanism the existing platform requirement gestures at and the one replay and prediction depend on.

## 7. The camera is four things, and conflating them is the usual mistake

| Concept | Is |
|---|---|
| **Camera definition** | An authored asset: rig composition and parameters |
| **Camera rig** | A runtime instance with state |
| **Evaluated camera** | The pose and lens resolved for a frame |
| **Render view** | The renderer's workload description |

They are separated because they have different lifetimes and different owners, and because one
evaluated camera can produce several render views — a main view, a weapon view, a portal, a
minimap. "One camera equals one framebuffer" is an assumption worth refusing early.

It also keeps renderer concerns out of gameplay: temporal jitter, backend clip conventions, and
reversed-Z belong to the render view, and a gameplay camera API that exposed them would leak the
backend into game code permanently.

## 8. Rigs compile, like everything else that composes

`ThirdPersonCamera` deriving from `Camera` is where camera code goes to become unmaintainable, and
every engine that offers the base class gets the hierarchy.

A rig is a **graph of nodes** — target, follow, orbit, constraint, collision, aim, shake, lens —
compiled to a compact program. This is the fifth time this pattern appears in the engine, after
materials, VFX, AI, and animation, and the argument is the same each time: authoring composes,
runtime evaluates a program, and no virtual node objects are allocated per camera.

## 9. Gameplay never writes a camera transform

If any system may set the camera's position, the final transform is whatever ran last, and nobody
can explain what moved it.

So gameplay emits **intent** and **impulses**: a look delta, a zoom, an explosion at a world
position with a strength. The camera system decides what those mean, and every contribution is a
layer with a weight — base rig, additive modifiers, shake, volume influence, cinematic override,
debug override. The debugger can then show which layer contributed what, which is only possible
because nothing bypassed the model.

## 10. The camera target is not the controlled entity

Assuming otherwise is convenient and wrong for most of the interesting cases: a strategy camera
frames a selected army, a spectator watches someone else, a player drives a character while the
camera follows a drone, a death camera looks at the killer.

So the target is a separate binding with its own policy — an entity, a group, bounds, a world
position, or a spline — and following the controlled entity is one policy among several rather than
the structure.

## 11. Collision and occlusion are different questions

A pane of glass occludes without colliding; a chain-link fence collides without occluding
meaningfully. A camera that treats them as one question either pushes in through things it should
see past, or leaves the player looking at a wall.

Both are specified: a **collision query** keeps the camera out of geometry, an **occlusion query**
keeps the target visible, and the responses differ — pull in, slide, swap shoulder, or fade the
obstacle. Queries are batched through the physics interface rather than scattered as synchronous
casts through rig nodes.

## 12. Aim has three meanings and shooters need all of them

**View aim** is where the camera looks. **Control aim** is where the player is aiming, which is what
travels in a command and what the server validates. **Weapon aim** is where the muzzle actually
points, which differs by weapon offset, recoil, and animation.

Collapsing them produces the two classic defects: shots that do not go where the reticle is, and a
server that validates against a camera ray the client could have manipulated. The camera is
explicitly **not authoritative gameplay state**.

## 13. Camera cuts are events with consequences

A cut invalidates temporal history, changes what illumination has converged, dirties shadow pages,
and moves the streaming source discontinuously. Every one of those systems already has a mechanism
for it; none of them can act without being told.

So a cut is a **typed event** carrying its reason, and a persistent view carries a **history
identity** so the temporal framework knows which history belongs to which view — changing a camera's
target is not a cut, while a hard cut is.

Anticipated cuts — a cinematic knows its cut list — become deadlines through the residency layer,
which is what turns a cut from a hitch into a prepared transition.

## 14. One evaluated camera, three downstream products

From one evaluated state come the **render view**, the **audio listener anchor**, and the **streaming
source**. They are derived rather than independently configured, so they cannot disagree about where
the player is — a listener stuck at the character while the camera is fifty metres away is a bug
that only exists when they are configured separately.

The streaming source carries velocity, frustum, and predicted trajectory, which is precisely what
the world, geometry, texture, and shadow systems have been specified to consume and have had no
producer for.

## 15. Simulation, render, or hybrid

Camera smoothing at simulation rate looks choppy at high refresh rates; camera evaluation at render
rate is not reproducible.

So evaluation mode is declared: **simulation** for reproducible and gameplay-coupled cameras,
**render** for purely visual ones, and **hybrid** — target state from simulation, smoothing at render
rate — as the recommended default for ordinary gameplay. Hybrid is what gives a smooth camera at
144 Hz over a 60 Hz simulation without making the camera part of the simulation's state.

## 16. Strategy cameras are first-class, not an example

The engine's own benchmark is a hundred-thousand-unit strategy frame. A camera system that treats
pan, zoom-with-tilt, edge scroll, terrain following, and map bounds as things a game implements on
top would fail its own acceptance test.

Terrain following in particular is worth specifying: querying terrain height directly rather than
raycasting physics every frame is available because the terrain capability exposes height queries,
and the difference at scale is substantial.

## 17. Build order

| Step | Contents |
|---|---|
| 1 | Device abstraction with timestamps; input users and device assignment |
| 2 | Action values, bindings, composites, context stack |
| 3 | Processors, modifiers, triggers, action lifecycle |
| 4 | Rebinding, profiles, control schemes, accessibility |
| 5 | Fixed-tick sampling and command frames; input debugger |
| 6 | Camera definition, evaluated camera, render view separation |
| 7 | Follow and orbit rigs; camera stack and blending; stable smoothing |
| 8 | Collision and occlusion; local player integration |
| 9 | Strategy camera; volumes; shake and recoil |
| 10 | Streaming source, listener, cut events, temporal integration |
| 11 | Rig compiler; camera debugger |

**Step 5 is the milestone that matters for input** — once actions resolve into command frames per
tick, replay and prediction have their substrate. **Step 6 matters for the camera**: separating the
evaluated camera from the render view early is what keeps renderer concepts out of gameplay for the
rest of the project's life.
