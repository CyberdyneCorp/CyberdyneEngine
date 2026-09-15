# `tools/editor/` — the play-mode and live-edit-policy contracts

`editor-architecture` and `live-editing` sat at **Seed** from M5 to M11 because two greps returned
nothing, and M11.b's first attempt to take them off it was those same greps inverted — "the token is
found somewhere". M11's gate called them *word-greps a dummy job satisfies*, which is the eighth
instance of the defect `tools/roadmap/falsify.py` enumerates.

| | |
|---|---|
| `play_contract.py` | Derives each side's table from the declaration that fixes it, and requires the sides to agree |
| `selftest.py` | Breaks each input on purpose and checks that the gate notices — eighteen cases |

```
just quality-editor-contract             both contracts against the tree
just quality-editor-contract --selftest  the gate's own negative cases
python3 tools/editor/play_contract.py play-modes
python3 tools/editor/play_contract.py live-edit-policy
```

Run by CI through the CTest entries `integration.editor_contract_play_modes`,
`integration.editor_contract_live_edit_policy` and `integration.editor_contract_gate`, and named by `m11b:play-modes-exist` and
`m11b:live-edit-policy-exists`. Neither needs a configured build.

## What each contract compares

**`play-modes`** — `src/gameplay/play/{include/cy/gameplay/play/mode.h,src/mode.cpp}` against
`editor/crates/cy-editor-viewport/src/play.rs`, against `openspec/specs/live-editing/spec.md`
(Play modes) and `openspec/specs/editor-architecture/spec.md` (Play mode). The enumerations, their
order, their spellings, `kPlayModeCount`, `PlayMode::ALL`, which modes can step a frame, which
isolate editor state, whether every refusal names the rung the mode is due at, and whether either
side can answer an unknown word with a mode rather than a refusal.

**`live-edit-policy`** — `src/gameplay/live/{include/cy/gameplay/live/policy.h,src/policy.cpp}`
against `src/core/reflect/include/cy/core/reflect/attributes.h` and the specification's own table.
The six policies, `kLiveEditPolicyCount`, the kebab-case spellings, the total order of
`live_edit_disturbance`, a derivation arm for every `PersistenceKind` — and the one that matters
most: `RecreateEntity` and `RestartWorld` are **never derived**. `policy.h` argues that at length and
`m11b:live-edit-applies-without-a-restart` rests on it; a build in which the classification could
produce them would make the per-field declaration unnecessary and that criterion vacuous.

## Why the tables are derived rather than sampled

The argument `tools/abi/README.md` makes about struct layouts, and it is why this needs no build: the
spellings are fixed **by the declaration**. `kNames` is `constexpr`, the Rust `name()` is a `const fn`
over a closed match, and both are exhaustive over an enumeration. Reading the declarations states
that rule; running a binary observes one instance of it. It also makes the two criteria provable by
`just roadmap-falsify`, whose sandbox is the tracked tree and not a build.

The behavioural half is **not** replaced by this and must not be. `m11b:play-mode-round-trip` drives
one world through all three modes in the compiled engine, and `m11b:live-edit-applies-without-a-restart`
edits a field of each policy class in a running one. This gate is what makes those suites' subject the
specification's subject — the half a passing suite cannot establish about itself.

## Why a parser that does not understand something is an error

The sixth of the seven unfalsifiable checks this repository found was a dependency gate that parsed
backticks out of a table written in **bold**, so it parsed nothing and compared nothing to nothing.
Every reader in `play_contract.py` raises `ContractError` when it finds nothing, `table_keys` refuses
a table with fewer than two backticked rows, and `selftest.py`'s last case writes that exact bold
table and requires exit 2 rather than a contract that holds.
