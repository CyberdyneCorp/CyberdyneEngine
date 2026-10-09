# Design: project characters and the bake (#29 Animation, #112's gaps)

## Context

#112's panel compiles, previews and edits pose graphs. Its preview character was a mannequin built in
code, because the design said the editor imported no skeleton. That was no longer true: the FBX
importer cooks a skeleton (step 7), the clips (step 8) and the skin (M11.b) into the project's cooked
directory. `cy/animation/cooked.h` (#99) reads those records without the importer, and
`AnimationLibrary` binds them into a rig.

The authored events lived only on the graph's `pose.clip` nodes. The preview gave them to its clips,
and nothing else read them. The editor's Play had no animation backend, so ABI 1.7's
`animation_*` calls from a Swift behaviour returned `CY_RESULT_UNAVAILABLE`.

## Decisions

### The engine loads the character; the editor names it

The editor finds characters and clips in the project's `.import` records, which the Content Browser
already reads. A character is a source with a `skeleton/` sub-asset; its mesh is that source's first
`mesh/` sub-asset; the clips are every `animation/` sub-asset in the project.

`animation.character.set` sends the ids. The engine reads the cooked records through the host's
`AnimationAssetSource` (the runtime reads `<project>/.cy/cooked/<id>.cyasset`) and decides what plays:
- a clip whose tracks mean another skeleton's joints (`clip_matches_skeleton`) is refused, and the
  reply names it and says why;
- a skeleton or mesh that does not load refuses the request and keeps the character that played.

The editor computes nothing about either.

### A clip is named by its sub-asset

Every Mixamo export names its one animation stack `mixamo.com`, and the cooked clip keeps that name.
The importer already names the sub-asset after the file when a file has one clip (`animation/Walking`),
because that is what the artist named. A graph names a clip by that leaf, and the preview and the bake
rename the decoded clip to it.

### The choice is a file beside the graph

CyberGraph text has no graph-level metadata, and a pose node for "the character" would be a node the
compiler must ignore. So the choice is a `cyanimcharacter 1` file, `<graph>.cyanimcharacter`, written
under the graph's own undo domain (`animation_graph:`). Undo and redo restore it as they restore the
graph, and the host's `animation_graph_changed` recognises it and shows the preview on the restored
character. No file means the mannequin.

### The character goes before the preview, once

`AnimationRequests` remembers the character it last sent. Every preview request the editor makes is
preceded by `ensure_animation_character`, which queues `animation.character.set` only when the graph's
choice differs from it. The queue keeps the order, so the preview is evaluated on the right skeleton.
The choice is resolved again only when the graph or its character file changed, because a scrub asks
every frame and listing the project's imports walks its tree.

The engine's answer changes the clip choices, so the vocabulary is requested again and every kept
compile is dropped.

### The bake cooks what a game loads, and the editor writes it

A program has no event table: a clip carries its events. `animation.bake` compiles the graph against
the character it names, not against whatever the preview plays. It then decodes each clip the program
names, clears its events (`Clip::clear_events`), adds the graph's in time order, sets the loop mode the
clip's node says, and re-encodes it. Re-encoding adopts the codec's keys as they were, so the motion
is bit-identical to the importer's. The program is encoded with `encode_program`.

Every record is wrapped as a cooked asset of kind animation. A `cyrig 1` manifest names the rig, the
character's skeleton and mesh by id, and each clip by name and file. The engine answers with the
files. The editor replaces `.cy/cooked/animation/<graph name>/` with them, so a clip the graph no
longer names leaves nothing stale.

The mannequin cannot be baked, because it has no cooked skeleton a game could load. A graph with an
error bakes nothing, and the reply carries the compile's diagnostics.

### Play is a game loading its rigs

`PlayAnimation` does what `samples/13-rts-api`'s host does with its worker:
1. puts the baked records into a memory mount under ids local to the session;
2. loads them through `AssetSystem` and `AnimationLibrary`, which binds the program's clip table by
   name and refuses a clip cooked for another skeleton;
3. registers each rig with an `AnimationSystem` over the Play world, under the rig's name.

The `AnimationAdapter` is bound into the behaviours' host before they start, so `onCreate` can attach
an animator. Each fixed tick runs:
1. the behaviours' fixed step;
2. `AnimationSystem::run` for one tick;
3. `AnimationAdapter::update` and `begin_frame`;
4. the behaviours' `frame_update`, which reads that tick's events.

The editor's Play had never called `frame_update`, so no behaviour's `onUpdate` ran in it. One frame
per fixed tick is the runtime's frame rate for scripts. A rig that does not load is skipped and named
on stderr, so one stale bake does not stop Play.

### Curves stay a follow-up

The shared timeline can key a value curve. But a clip's curve tracks (`Clip::add_curve_track`,
`sample_curve`) have no reader: no pose graph node and no ABI entry consumes them. A curve authored on
the timeline would reach nothing a game runs, so it is listed, not built.

## Not built here

- Retargeting a project clip cooked for another skeleton (it is refused).
- A baked rig in the content cook and its packages (Play loads `.cy/cooked/animation/`).
- Play's viewport drawing the animated characters.
- Curve editing on the timeline.
- The skinned preview on Metal and D3D12.
