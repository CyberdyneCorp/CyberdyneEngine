# Correct M8's artefact claim, and record its three-way split in the ladder

## Why

**M8.c's closing gate handed this forward and M9's closing gate is performing it.** M9's task 7.6
names it in as many words: `delivery-roadmap`'s milestone table gives M8's closing artefact as

> A playable vertical-slice game exercising **every gameplay-facing capability at Working**

and `vfx-system` exits M8.c at **Seed**, with its Working cell moved to M10 by M8.c's own gate rather
than claimed. `docs/roadmap/status.yaml` records it at `seed`, `docs/roadmap/capability-matrix.md`
carries `W` in M10's column, and `docs/ROADMAP.md`'s M10 row says why: the GPU compute dispatch M8.c
did not build. So the specification's sentence is false about the milestone that has closed, and it
is false in the direction that matters — it claims more than happened.

**Correcting the sentence is not the same as claiming the tier**, and that distinction is the whole
reason this is a change against the specification rather than an edit to a document. The artefact
`samples/08-vertical-slice` exists, is playable, is measured and is photographed. What it does not do
is exercise a capability whose default path was not built.

**And the same row has a second, older inaccuracy.** M8 was split into **M8.a**, **M8.b** and **M8.c**
by `split-m8-authorable-and-systems` and `split-m8b-defer-spectacle`; `tools/roadmap/record.py`'s
`MILESTONES`, `docs/ROADMAP.md` and the capability matrix all carry three rungs where this table
carries one. M5.5's insertion is recorded in this requirement in a paragraph of its own, with its
reason. M8's split is recorded nowhere in it. The ladder that is meant to be authoritative is the one
document that does not know M8 was three milestones.

## What Changes

- **MODIFIED** `delivery-roadmap` → "The milestone ladder": the M8 row's closing artefact becomes an
  accurate statement of what M8 closed on, naming `vfx-system` as the capability it did not reach and
  the milestone that carries it now; and a paragraph records the three-way split the way M5.5's
  insertion is recorded, so that the ladder in the specification and the ladder in
  `tools/roadmap/record.py` describe the same twelve rungs.
- No tier moves, no capability is re-sequenced, and no exit criterion changes.

## Impact

- **Specification**: `openspec/specs/delivery-roadmap/spec.md`, one requirement.
- **Documentation**: none. `docs/ROADMAP.md` already carries M8.a, M8.b and M8.c as separate sections
  with their own records, and already says `vfx-system` reached Seed.
- **Code**: none.
