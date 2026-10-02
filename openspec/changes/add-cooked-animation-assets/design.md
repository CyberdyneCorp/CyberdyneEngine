# Design

## The records

| Record | Version | Writer | Reader |
|---|---|---|---|
| skeleton | 1 | importer step 7 (`write_cooked_skeleton`), runtime `encode_skeleton` | runtime `decode_skeleton`, importer `read_cooked_skeleton` |
| clip | 1, and 2 with markers and events | runtime `encode_clip` (the importer's `write_cooked_clip` calls it) | runtime `decode_clip`, importer `read_cooked_clip` |
| program | 1, magic `CYPG` | runtime `encode_program` | runtime `decode_program` → `assemble_pose_program` |

The skeleton writer stays in the importer as well because step 7's bytes must be the same with
`CY_ANIMATION=OFF`, where the runtime does not exist; `integration.animation_cook` holds the two to
one record by re-encoding the importer's bytes and comparing.

A clip carries the names of the joints its tracks index. `AnimationRig::bind` checks only counts, so
`clip_matches_skeleton` is the check that a clip means this skeleton's joints.

## No compiler in the runtime

`lower_pose.cpp` held the program's storage, `advance`, the reference evaluator and the compiler.
The first three move to `pose_program.cpp` with `assemble_pose_program`; the compiler stays. A
static library contributes only the objects a link needs, so a runtime that loads programs pulls
`pose_program.o` and never `lower_pose.o`. `unit.animation_runtime_only` makes it a link-time fact:
it defines `compile_pose` itself, so linking the real one would be a duplicate definition.

## The library

`AssetSystem` loads bytes; `AnimationLibrary` is the typed layer. Each id is loaded and decoded once
and kept for the library's life. A rig is bound by name; every refusal is an `Error` whose message is
the library's last refusal text, also emitted as an `animation` diagnostic.

Hot reload: on `AssetSystem::reload`, a clip is decoded beside the live one, checked against every
skeleton it is bound to, then moved into the same `Clip` object and its rigs rebound so a changed
duration reaches the clocks. A cursor sized for the old track count resets itself on its next
sample. A failed reload keeps the working clip. Skeletons and programs are not swapped under live
instances — their state is laid out from them — and their reloads are refused with a diagnostic.

## The cook

`cook_locomotion_set` reads each source's skeleton and clip with the runtime decoders, uses a clip
as it is when its rig is congruent with the character's and shares its rest pose, retargets and
bakes it otherwise, and compiles the locomotion machine over the clips' names and durations. The
`animation` producer runs it as a node whose sources are a `cyanim 1` description and whose
upstreams are the import nodes; outputs are matched by suffix.
