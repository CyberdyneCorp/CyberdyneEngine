# Design: the editor's animation panel (#29 Animation, #76 stage 5)

## Context

`cy::graph::pose` compiles a pose graph — clips, blends, states, transitions — into a `PoseProgram`, and
`cy::animation` evaluates one: `advance` moves the state machine and the clip clocks, `evaluate`
samples and blends, the skeleton turns the local pose into skinning matrices. #99 put that in the frame
and #107 drew skinned meshes through it. The editor has the Wave 0 scaffold (#59), the shared graph
canvas, a shared timeline widget no panel had drawn, and the gameplay graph editor (#98) as the pattern
for an engine-owned vocabulary, an engine compile, and canonical CyberGraph text.

## Decisions

### The engine previews; the editor asks

The preview is `cy::editor::AnimationPreview`, an engine object, reached through
`animation.preview.set`: the graph's text, a focus (a `pose.clip` node, or zero for the state machine),
a time, playing or not, and the author's parameters. The service compiles the text exactly as
`animation.compile` does and hands the program to the preview, which evaluates it with `cy::animation`
and keeps the pose, the skinning matrices and the events crossed. The editor keeps only what it asked
to see (`PreviewSettings`) and the engine's last answer. Nothing in the editor samples or blends.

A state machine preview at time `t` is the program advanced from its entry state in steps of
`kAnimationPreviewStep` (a sixtieth of a second, the last step shorter), so a scrub lands on the pose a
run of the same steps reaches, and the engine's suite checks the preview against exactly that run made
directly with `cy::animation`. A clip preview samples the clip at `t` (`sample_unwrapped`, so the end of
a looping clip shows its last frame, not its first). A playing preview is advanced by the host each
frame: a clip wraps or holds as a game's does; a state machine runs in whole steps and starts again
after `kAnimationGraphPreviewSeconds`.

### The preview character is built in code

The editor imports no skinned model yet (model import stops before the skeleton step), so a project's
own character cannot be previewed. The preview character is a twelve-joint mannequin with four clips
(`idle`, `walk`, `run`, `wave`) and one box per bone, built the way `samples/13-rts-api`'s worker is.
Its clips carry no events of their own: a clip's events are the graph's, so what fires is what was
authored. Previewing a project's cooked character is a later slice.

### A clip's events are its node's `events` property

A cooked `PoseProgram` has no event table — events belong to the clip asset — and a pose graph has no
clip asset of its own to write. The events live on the `pose.clip` node, as `name@seconds` items
separated by `; `, written in time order with C's `%.9g`. The engine parses and checks them (a name it
can read, a time inside the clip) and gives them to the clip it previews. The timeline draws one event
track per name, a key per time; `animation.event.add`, `.move` and `.remove` change the one property, so
each is one undoable transaction on the graph's file. Writing the authored events into a cooked clip is
the cook's later slice.

### A cut is refused when it is authored

`compile_pose` reads a transition of zero duration as a cut, and `request_state` keeps that for a game
that asks for one. An authored transition with no positive blend is refused by `animation.compile` on
that transition (`animation.transition.cut`), as `build_locomotion_graph` refuses it: a cut between two
poses is a visible pop, and an author who leaves the field at zero gets it silently otherwise. The
compiler itself is unchanged.

### The vocabulary validates as drawn

The canvas wires an output into an input of the same type. The compiler reads a transition's `from` and
`to` from whatever is wired into them, and the builders wired a state's `pose` INPUT there, which
`validate` reports as a wire of the wrong direction and no canvas can draw. `pose.state` now declares a
`state` output of type `state`; the builder wires from it. A clip node from the palette names no clock,
and the compiler interned every such clip's clock as the empty name — one clock for all of them; an
unnamed clock is now the node's own.

### The frame draws a skinned preview

`AuthoredFrame::set_skinned_preview` takes a mesh (positions, normals, four influences per vertex,
indices) and per-joint matrices. The mesh joins the rigid streams (where a skinned draw reads its UVs)
and a one-mesh `SkinnedScene`; each frame uploads the matrices, the scene declares its dispatch before
the frame's passes, and every pass that draws it reads the skinned output. The preview has its own
reserved material slot and spatial entry. The skinned pipelines are created only where #76 has run them
(Vulkan); elsewhere the preview is refused by name and the runtime does not offer one.

### One request in flight, the newest preview wins

`AnimationRequests` follows `ScriptRequests`: the catalogue once per connection, compiles kept per
reference with the source they answered. A queued `animation.preview.set` is replaced by a newer one,
so a scrub drag sends one request per frame and the engine evaluates the last. While the panel is drawn
and the preview plays, its state is polled ten times a second so the playhead follows the engine.

### A busy service request waits instead of being dropped

Driving the panel against the real runtime found that the runtime ignored `submit`'s result. The
editor service takes one request at a time per backend, the editor keeps one request in flight per
kind of request, and two kinds reach one backend in one frame (the animation catalogue and the VFX
catalogue do, at connection). The refused request was never answered, and every request manager gated
on that one (audio, scripts, animation) stopped. `PendingServiceRequests` in the hosted runtime keeps
a refused request and submits it in arrival order once the service is free; a request refused for any
other reason is answered as failed, and a waiting request the editor cancels is answered as cancelled.

## Not built here

- Previewing a project's own skinned character; writing authored events into cooked clips.
- Blend1D/Blend2D and the other nodes #92 tracks; curve editing on the timeline.
- The skinned preview on Metal and D3D12 (their skinned pipelines have not run on a device).
