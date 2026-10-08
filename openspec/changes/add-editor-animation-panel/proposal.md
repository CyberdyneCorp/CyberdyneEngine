# Proposal: The editor's animation panel (#29 Animation, #76 stage 5)

## Why

Issue #29 lists Animation as High for the M12 RTS, and issue #76 left the panel as its stage 5. The
engine side exists: pose graphs compile to a `PoseProgram` (`cy/graph/lower_pose.h`), the animation
system evaluates them each frame (#99), cooked programs load without the compiler (#99), and skinned
meshes draw in the forward frame (#107). The editor declares `Domain::AnimationGraphsAndClips` with
the nine `pose.*` names and an animation timeline, but no panel opens it: a pose graph can only be
written in code, a clip's events cannot be placed, and nothing previews a graph.

## What Changes

- **The pose vocabulary validates as an editor draws it.** `pose.state` gains a `state` output, which
  is what a transition's `from` and `to` are wired to, so a drawn state machine passes
  `graph::validate`; the locomotion builder wires its transitions from it. A `pose.clip` that names no
  time parameter gets a clock of its own (`clock.<key>`) instead of sharing the empty-named one with
  every other unnamed clip.
- **The backend service.** `animation.catalogue.get` (the pose vocabulary in the material catalogue's
  schema 3, the preview character's clips as a clip node's choices), `animation.compile` (`validate`
  and `compile_pose`, plus the authoring checks a program cannot carry: a transition wired to fewer
  than two states, a cut, no condition, a state with no pose, an unknown clip, an event that does not
  parse or lies outside its clip), and `animation.preview.set`, `.get` and `.stop` on `MaterialService`,
  through a host seam `cy::editor::AnimationPreviewRuntime`.
- **The preview character, evaluated by the engine.** `cy::editor::AnimationPreview` (built with
  `CY_ANIMATION`): a twelve-joint mannequin, four clips and one box per bone. It previews a graph's
  state machine — advanced from the entry state in sixtieths of a second with the author's parameters
  — or one clip alone, gives each clip the events its node authors, reports the events playback
  crosses, and hands a host its skinning matrices.
- **Drawn by the engine.** The hosted runtime's `AuthoredFrame` draws a `SkinnedPreview` through a
  `SkinnedScene` and the frame's skinned pipelines (Vulkan, where #76 drew them), and advances a
  playing preview each frame.
- **No editor request is dropped.** The hosted runtime used to ignore a submission the service
  refused because it was busy with another, so that request was never answered; it now waits its turn
  (`PendingServiceRequests`), and one refused for another reason is answered as failed.
- **The editor.** An Animation panel on the specialised scaffold, the shared canvas and the shared
  timeline: the engine's palette; the clip on the timeline with one event track per event name;
  Play, Pause, Stop and a scrub on the ruler; the author's parameters; the engine's diagnostics on
  their nodes. `animation.graph.create`, `animation.node.{add,move,connect,disconnect,remove,
  property.set}` and `animation.event.{add,move,remove}` are each one undoable transaction and an MCP
  tool; `animation.graph.{read,compile}`, `animation.preview.{scrub,play,pause,parameter,stop}` and
  `animation.status` are reads. The `.cyanimgraph` is the engine's canonical CyberGraph text, and an
  edit, an undo or a redo of the previewed graph previews it again.

## Capabilities

### Modified Capabilities

- `editor-architecture`: the animation graphs and clips editor.
- `animation-and-skinning`: the pose vocabulary an editor draws, and a clip's own clock.

## Impact

- Engine: `src/graph/` (`register_pose_nodes`, `lower_clip`, the locomotion builder's wires),
  `src/editor_backend/` (`animation_service`, `animation_preview`, `graph_wire.h`, `wire::Writer`;
  `MaterialService::set_animation`; the `animation.` prefix; `capabilities.get` lists the five
  operations), `samples/05b-editor-window/runtime/` (`AuthoredFrame::set_skinned_preview`; the host
  ticks and draws the preview; links `cy::rendering-skinning`).
- Editor: `cy-editor-services` (`animation_graph`, `animation_requests`, `animation_commands`;
  `ProjectHost` gains seven defaulted methods and `AnimationPreviewChange`), `cy-editor-interface`
  (`specialised::animation`, `animation_authoring_commands`, `install_animation_catalogue`; the
  animation timeline offers gameplay event tracks), `cy-editor-shell` (`panels/animation.rs`; the
  default workspace's Animation tab opens it; the timeline keys an event track on a double-click).
- No ABI or bridge message change.
