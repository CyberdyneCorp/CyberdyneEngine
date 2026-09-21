# M12 — The Game: a vertical slice that proves the engine by using it

## Why

**Sixty capabilities are recorded Working, each verified in isolation, and nothing has ever used
them together.** That is the gap no audit closes. `m11b:the-game-exists` has been a declared gap
since M11.b — its check is `ls -d samples/*game*` and its answer is nothing — along with
`-is-playable`, `-drawn` and `-is-honest-about-its-content`. Four criteria, one absent subject, and
no rung of the ladder built it: M11.e's artefact is `samples/11-ship`, which `m11b:the-game-exists`
itself calls *"a packaging proof"* rather than the game.

**A game is the observation this ladder has been asking for the whole way.** Its own rule is that a
claim nobody observed is not a claim, and eleven checks have shipped here unable to fail. Sixty rows
at Working are sixty claims observed one at a time.

## What changes

- **M12 becomes the rung that builds the slice**, and the four game criteria move here from M11.e.
  `delivery-roadmap`'s *"A criterion follows its subject"* requires it: leaving them behind produces
  a check with nothing to check.
- **1.0 therefore stops claiming a game.** This is the deliberate part. **1.0 is the engine; M12 is
  the proof it works.** M11.e records that in the 1.0 statement rather than leaving a reader to infer
  it, and the four criteria say plainly which rung answers them.
- **Android, `ml-inference` and `xr-support` move to M13**, which takes the rung M12 previously held.

## The slice is scoped by what it exercises, not by what it contains

**Art, balance, campaign and polish are out of scope.** Each clause below exists because it drives a
capability that is currently claimed and unobserved:

| The slice does this | Because it exercises |
|---|---|
| 200 units selected and ordered across one map | `navigation` flow fields — whose header claims 20,000 agents to one destination — plus crowd avoidance |
| Units drawn at unit count without stalling | instancing, indirect draws, GPU culling, HLOD, virtual geometry |
| Fog of war, an economy, a victory rule | `gameplay-framework`, which is recorded **Complete** and whose spec requires exactly this scenario |
| Two clients, same seed, same result | `simulation-and-determinism`, `networking-and-replication`, `replay-and-rollback` |
| Authored in the editor where the editor can author | the four domains that have a vocabulary, and an honest record of the twelve that do not |

## And every defect found gets a regression test

The second half of this rung is not "fix bugs". **It is: each defect the slice finds is repaired in
a change that carries a test which fails without the repair.** A count of repairs is not a gate; a
count of tests each watched going red is. That is the project's own standing rule, applied to the
first real consumer of the engine.

## What this rung is not

It is not a game project. It is a **vertical slice sized to exercise named rows**, and it finishes
when those rows have been observed together — not when the game is good.
