# Proposal: A project's own characters in the animation panel, and its events in the game

## Why

PR #112 built the editor's Animation panel (#29, #76 stage 5) and listed what it left out. The
preview played only a built-in mannequin, never a character the project imported. The events an
author placed on the timeline drove that preview, but no clip a game loads carried them. So a Swift
behaviour could not receive an event placed in the editor. The editor's Play had no animation at all:
no rig was registered, and behaviours had no frame callback in which to read events.

## What Changes

- **The project's characters.** `cy::editor::AnimationCharacter` is either the mannequin or a project
  character. A project character is the cooked skeleton, skinned mesh and clips the importer wrote for
  an FBX (steps 7 and 8, and M11.b's skin), decoded with `cy/animation/cooked.h` through a host's
  `AnimationAssetSource`.
  - A clip cooked for another skeleton is refused by name. The rest are kept.
  - A clip is named by its sub-asset's leaf (`animation/Walking` is `Walking`), not by the FBX stack's
    name.
  - A model with no skin is drawn as one box per bone.
  - `animation.character.set` chooses the character. Its clips then become the catalogue's clip
    choices and what a compile checks against.
- **The bake.** `animation.bake` compiles a graph for its character and cooks the rig a game loads
  (`cy::editor::AnimationRigBaker`). The rig is:
  - the program;
  - every clip the program names, renamed as the graph names it, looping or holding as its node says,
    with the graph's authored events in place of the clip's own;
  - a `cyrig 1` manifest.
  
  `Clip::clear_events` makes the replacement possible.
- **Play animates the baked rigs.** In the editor window's runtime, `PlayAnimation` loads every rig in
  `.cy/cooked/animation/` through the asset system and `AnimationLibrary`, registers it by its graph's
  name, and binds ABI 1.7's animation backend into the Swift behaviours. Each fixed tick then runs:
  1. the behaviours' fixed step;
  2. one animation tick;
  3. their `onUpdate` with that tick's events (`ScriptRuntime::frame`, which Play did not call before).
- **The editor.**
  - `animation.character.list` reads the project's `.import` records.
  - `animation.character.set` writes `<graph>.cyanimcharacter` as one undoable transaction. Its undo
    and redo show the preview on the character they restore.
  - `animation.bake` sends the bake and writes the answer, replacing the rig's directory.
  - The character is sent to the engine before the preview that needs it, and only when it changed.
  - The panel gains a Character row, the engine's account of the character, and Bake.

## Capabilities

### Modified Capabilities

- `editor-architecture`: the animation editor's project characters and bake.
- `animation-and-skinning`: authored events reach a game's clips.

## Impact

- Engine:
  - `src/animation` (`Clip::clear_events`).
  - `src/editor_backend`:
    - `animation_character`, `animation_rig`, `animation.character.set` and `animation.bake`;
    - `MaterialService::set_animation_baker`;
    - `AnimationPreviewRuntime` gains the character methods, and `AnimationClipCatalogue` is what a
      graph compiles against;
    - `capabilities.get` lists seven operations.
  - `samples/05b-editor-window/runtime`: `animation_assets`, `play_animation`,
    `ScriptRuntime::bind_animation` and `frame`, and the Play glue.
  - `project/game/AnimatedHero.swift`.
- Editor:
  - `cy-editor-services`: `animation_graph`'s character and bake wire, `animation_requests`, the
    new `animation_character` module and the three commands.
  - `cy-editor-commands`: four defaulted `ProjectHost` methods and `AnimationCharacters`.
  - `cy-editor-shell`: the panel's Character row and Bake.
- No ABI or bridge message change.
