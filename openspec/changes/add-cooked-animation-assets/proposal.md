# Proposal

## Why

There is no runtime loader for a cooked skeleton, clip or program. The readers live in `tools/import`,
a cook-time library that `samples/09b-animated-character` links directly; `AssetSystem` has
`AssetKind::Animation` and no loader behind it; and a `PoseProgram` has no cooked form at all — the
sample compiles its machine at startup with `compile_locomotion`, which breaks the specification's
rule that "the runtime SHALL contain no graph compiler" as soon as a shipped game needs one. Issue
#76 stage 2.

## What Changes

- `cy::animation-assets` (`src/animation/assets/`): `cooked.h` decodes and encodes the importer's
  skeleton and clip records (byte for byte the importer's), and a NEW cooked pose program record.
  Clip keys are adopted as the codec stored them (`Clip::adopt_compressed`), not re-fitted.
- `graph::pose::assemble_pose_program` rebuilds a cooked program, validating every index, and lives
  with `advance` in a new `pose_program.cpp`, split from the compiler, so a runtime that loads
  programs links no `compile_pose`. A link-time test proves it.
- `AnimationLibrary`: loads the three kinds by asset id through `AssetSystem`, binds a rig by
  matching the program's clip table BY NAME (a miss is refused naming the clip), refuses a clip
  cooked for another skeleton (naming the joint), and swaps a hot-reloaded clip into the same object
  so rigs and instances keep their pointers.
- `cy::import::cook_locomotion_set` (`tools/import/animation_cook.h`) cooks a character from
  imported sources — skeleton, retargeted and baked clips, compiled program — and the `animation`
  build-graph producer runs it over upstream import bundles from a `cyanim 1` description.
- The importer's clip writer becomes the runtime's encoder, so one writer produces the record.
- `samples/09b-animated-character` cooks with `cook_locomotion_set`, loads by asset id through the
  asset system and `AnimationLibrary`, and animates through the stage 1 system.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `animation-and-skinning`: cooked animation assets are added as a requirement.

## Impact

- `src/graph/` (the program's runtime half moves to its own file), new `src/animation/assets/`,
  `tools/import/` (the cook, the clip writer), `tools/build/` (the `animation` producer),
  `samples/09b-animated-character`.
