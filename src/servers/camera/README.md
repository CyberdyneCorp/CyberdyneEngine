# `src/servers/camera/` — layer 2

**CyberCamera**: composable rigs that produce an evaluated camera, from which a render view, an audio
listener anchor and a streaming source are derived.

**Governed by**: `camera-system`, which reaches **Seed** at M4 — tasks 4.3.1, 4.3.2 and 4.3.3.

## The one rule, and the layer number that enforces it

`camera-system` opens with two sentences that shape every signature in this module:

> A camera SHALL be addressable as a lightweight handle and data, and SHALL NOT require an ECS
> entity, a node, or a base class to exist. There SHALL NOT be a camera base class intended for
> subclassing to produce camera behaviours.

So there is no `Camera` class to inherit from, no virtual `update()`, and no entity in any parameter.
A rig is a handle into a pool, a definition is a compiled program, and an evaluated camera is a
value. Layer 2 sits *below* `src/backends/` and `src/scene/`, so this module **cannot** name an
entity, a node, a world or a physics query even if somebody wanted to — the layer checker fails the
build over it.

What that costs a caller, stated once: **the camera server cannot resolve a target**. Whoever owns
the world turns each `TargetBinding` into a `TargetSample` and passes the samples into `evaluate()`.
That is not a workaround; it is `camera-system`'s own "rig nodes SHALL NOT retain raw pointers to
component data across frames", made impossible to violate.

## The four separated concepts

| | is | lives in |
|---|---|---|
| **Camera definition** | an authored graph of rig nodes, compiled to a program | `rig.h` (`RigDefinition`, `compile`) |
| **Camera rig** | a runtime instance holding state | `rig.h` (`RigState`), `server.h` |
| **Evaluated camera** | the pose, lens and derived data for one frame | `camera.h` (`EvaluatedCamera`) |
| **Render view** | the renderer's description of a view | `cy::render::ViewDescription`, produced by `view.h` |

The evaluated camera deliberately has **no handle**: it is a value, recomputed every evaluation, and
giving it an identity would invite something to keep one across a frame boundary and read a pose that
no longer holds.

## What is here

| file | what it holds |
|---|---|
| `handles.h` | the three object families, as generational handles |
| `lens.h` | the gameplay and physical lens, one abstraction, and the blend rule |
| `smoothing.h` | half-life smoothing: frame-rate independent, resettable on a cut |
| `camera.h` | targets, intent, impulses, cuts, the listener anchor, the streaming source, the three aims |
| `rig.h` | node kinds, the authored graph, the compiled program, evaluation and its trace |
| `stack.h` | one player's contributions, their priorities, weights, blends and the report |
| `view.h` | render view production, and the history identity a cut invalidates |
| `server.h` | the pools, the impulse bus, the query batch, the settings, and evaluation |

## Four decisions worth knowing before changing anything

**A physical lens does not interpolate as though it were a field of view.** Half way between an 18 mm
and a 50 mm lens is 34 mm — 38.88 degrees — not the 47.19 degrees a field-of-view lerp gives.
`lens.h` carries the reasoning and `tests/test_lens.cpp` carries the numbers, so a blend "simplified"
to one lerp fails rather than merely looking faster at the wide end.

**Smoothing is a half-life, and the test is the composition property.** Two steps of `dt` leave the
same residue as one step of `2·dt`; `lerp(current, target, k)` does not. A test that pinned the
residue after a single step would pass for both, which is why `tests/test_smoothing.cpp` asserts the
composition and the equal settle time at 60 and 144 hertz instead.

**Collision and occlusion are published as a batch, never cast.** A `Collision` node appends a
`CameraQuery` and consumes the answer the caller left from the previous evaluation. `camera-system`
requires the batching; layer 2 is what makes the alternative impossible.

**One number is both halves of the history identity.** `history_identity()` mixes the camera's
identity, the view's key and the cut epoch. A target change moves none of the three, so history
survives it; a cut bumps the epoch, so every view of that camera is invalidated. A separate
"invalidate" flag would have to be delivered to every consumer, and the one that forgot would smear
across the cut.

## What Seed means here

**Real and exercised**: the four concepts and their handles; the rig graph, its compiler and its
diagnostics; follow, orbit, offset, look-at, lens, noise and constraint evaluation; the lens model in
both authoring forms; the camera stack with per-channel blend policies and the contribution report;
cuts; the listener anchor and streaming source, derived; render view production; the impulse bus; the
batched query protocol; the custom-node extension point.

**Interface only, and named so nobody mistakes it for more**: collision and occlusion *responses*
apply results the caller supplies — the physics server that answers them arrives beside this module,
and the batching contract is the part that had to exist first.

**Absent, not stubbed**: framing and composition constraints, camera volumes, the strategy camera,
the director camera, aim assistance and screen/world projection. All are `camera-system`'s and all
are M8's. None is stubbed, because a stub that returns a plausible pose is worse than an absent
function a caller cannot call.

## What is deliberately elsewhere

* **The view's matrices** — `cy::render::View::refresh()`. This module produces the *semantic*
  projection; the renderer builds the matrix, which is the one place reversed-Z is applied.
* **Temporal jitter** — the temporal framework's. `camera-system` requires the camera to provide an
  unjittered projection, and there is nowhere here to put an offset.
* **Resolving a target** — whoever owns the world. See the rule above.
