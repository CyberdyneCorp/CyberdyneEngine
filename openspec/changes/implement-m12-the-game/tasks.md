# Tasks — M12, The Game

## 0. The rung exists on the ladder, in all four places

- [ ] 0.1 `record.MILESTONES` carries `m12` between `m11e` and `m13`
- [ ] 0.2 `gates.toml` carries `milestone-m12` at `joins-on-close`
- [ ] 0.3 `tools/roadmap/milestones/m12.toml` exists and `just roadmap-milestone m12` reads it
- [ ] 0.4 This change directory exists and validates `--strict`
- [ ] 0.5 The four game criteria have MOVED here from `m11e` rather than being copied — a criterion
      follows its subject, and two ledgers asserting one subject is how a rung closes on the other's
      evidence

## 1. The slice, scoped by what it exercises

- [ ] 1.1 `samples/12-rts/` — one map, two factions, a win condition. **Art, balance and campaign are
      out of scope**; this is sized to exercise rows, not to be good
- [ ] 1.2 **200 units selected and ordered across the map**, moving on `navigation`'s flow fields
      rather than 200 A* queries. The row's own header claims *"20,000 agents ordered to the same
      destination"* guided by one field — this is the first time anything has tested that sentence
- [ ] 1.3 **Fog of war, an economy and a victory rule.** `gameplay-framework` is recorded **Complete**
      and its specification requires a scenario composing exactly *"base strategy, economy, fog of
      war, and a victory rule"* — so either the row supports it, or a Complete row is about to be
      contradicted by its first consumer, which is a finding worth more than the feature
- [ ] 1.4 Gameplay written in **Swift**, against `CyberdyneKit`, with `swift_reload` hot reload used
      in anger rather than in a test
- [ ] 1.5 Authored in the editor wherever the editor has a vocabulary — models, animation, abilities,
      scripting graphs — and **the README records what had to be authored outside it**, which today
      is terrain, water, foliage, VFX and UI layout

## 2. The measurements the slice exists to take

- [ ] 2.1 **Draw calls and frame cost at unit count.** `testing-and-quality` names an unwritten
      acceptance scenario called **strategy stress**; this is it. 200 units, counted draw calls,
      measured frame, against the same 16.7 ms budget the world demo now meets
- [ ] 2.2 **Two clients, same seed, same result** — and on two ARCHITECTURES if the arm64 machines are
      available, which closes `m11a:lockstep-agrees-across-architectures` as a by-product
- [ ] 2.3 **Physics fixed-step integration under load**, because an RTS desyncs if it drifts and the
      requirement is currently mapped by nothing
- [ ] 2.4 Every measurement is committed as evidence, not reported in a message

## 3. Every defect gets a regression test

- [ ] 3.1 **Each defect the slice finds is repaired in a change carrying a test that fails without
      the repair.** A count of repairs is not a gate; a count of tests each watched going red is
- [ ] 3.2 A running register of what the slice found, in this change's `design.md`: the defect, the
      row it belongs to, the test, and whether the row was recorded Working or Complete when it was
      found. **A defect in a Complete row is a finding about the ladder, not only about the code**
- [ ] 3.3 Defects that are capability gaps rather than bugs are declared as gaps naming their rung,
      not fixed quietly inside a game sample

## 4. The four inherited criteria

- [ ] 4.1 `the-game-exists` — `samples/12-rts/` is a game with authored content, not a harness
- [ ] 4.2 `the-game-is-playable` — a start state, a win and a loss, reachable headless
- [ ] 4.3 `the-game-drawn` — a committed screenshot under `docs/design/images/`, because a picture is
      the only evidence that separates a game from a test harness reporting that it won
- [ ] 4.4 `the-game-is-honest-about-its-content` — the README says what was authored, what was
      generated, and what the engine could not author at all

## 5. Records and the gate

- [ ] 5.1 M11.e's 1.0 statement says plainly that **1.0 is the engine and M12 is the proof**
- [ ] 5.2 `docs/roadmap/capability-matrix.md` and `ROADMAP.md` carry M12 and M13
- [ ] 5.3 Every criterion this rung adds has been shown able to fail by `just roadmap-falsify`
- [ ] 5.4 The gate
