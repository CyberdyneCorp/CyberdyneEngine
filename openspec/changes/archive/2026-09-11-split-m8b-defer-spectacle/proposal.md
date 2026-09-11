# Defer VFX, cinematics and inference to M8.c

## Why

**M8.b got bigger after its spike, not smaller.** It was already the largest rung on the ladder —
fourteen capabilities, nine of them from nothing. Its spike then refuted the premise the plan rested
on: one graph IR cannot serve seven consumers, and the answer is one authoring layer, one shared
expression core and **six compilers**. That is more work than the plan assumed, discovered after the
plan was written.

The same rule that split M8 applies again, and it is already in `delivery-roadmap`: *a milestone
whose artefact cannot be reached without its own risk spike succeeding, when some other coherent
artefact could be reached without it, contains two.* This is the weaker sibling of that test — the
spike succeeded and told us the milestone is larger than it looked — but the remedy is the same one,
for the same reason: **the half that makes a game playable should not wait on the half that makes one
look finished.**

**The seam is obvious once the dependencies are read.** Nothing M8.b keeps requires anything it
defers. `animation-and-skinning` names VFX only as a *consumer* of its curves and its pose world;
`camera-system` calls sequences "the principal producer of anticipated cuts" and works without one;
`gameplay-framework` treats a sequence as one origin of commands and requires that "the simulation
SHALL NOT be able to distinguish their origin"; and `ml-inference` is already recorded as
"`ai-system`, optionally". Every reference is a consumer relationship, not a prerequisite.

## What Changes

- **M8.c · Spectacle** is inserted after M8.b: `vfx-system` → Working,
  `sequencing-and-cinematics` → Working, `ml-inference` → Seed.
- **M8.b keeps everything a game needs to be played**: the shared authoring layer, gameplay and
  abilities, animation, AI and navigation, camera, the interface, text, 2D, and audio — plus the
  debts it inherited: prefab apply-and-extract, assembling the renderer, and giving `MeshRenderer` a
  reflected type.
- **The determinism firewall moves with its subjects.** M8.b's exit criterion "VFX and inference
  cannot write gameplay state, proven by a test" becomes M8.c's, because both subjects are deferred
  and a criterion with no subject is a criterion nothing checks.
- **M8.b's closing artefact loses two words.** `samples/08-vertical-slice` becomes: a level,
  characters that animate and think, abilities with effects, a heads-up interface, sound, and a 2D
  menu. The cinematic and the particles move to M8.c, which layers them onto the same slice rather
  than building a second one.
- **The insertion is recorded rather than renumbered**, as M5.5's and M8.a/M8.b's were, so every
  reference to M9 through M11 stays valid.

## Capabilities

### Modified Capabilities

- `delivery-roadmap` — the ladder gains one entry, and the rule that produced this split is widened:
  a milestone may also be split when **its own spike reveals it is larger than the plan assumed**,
  not only when a risky half blocks a settled one.

## Impact

- **Roadmap**: M9 through M11 keep their numbers and their content.
- **The ledger**: `record.MILESTONES` gains `m8c` in position, with the rung test the M8 split added
  already covering it.
- **Risk**: deferring reduces M8.b's scope by three capabilities and removes nothing any remaining
  capability depends on. What it costs is that the vertical slice has no particles and no cutscene
  until M8.c — which is a slice that plays rather than one that presents.
