# samples/09-multiplayer — M9's closing artefact

*A four-player session with rollback under packet loss; a replay that reproduces it bit-exactly; an
injected divergence narrowed to a field.*

```
just run-multiplayer                      # build, play, judge, and draw the two pictures
just run-multiplayer --only session       # one act
just run-multiplayer --shots docs/design/images   # refresh the committed pictures
just run-sample multiplayer --ticks 120   # the program alone, printing what it did
```

`integration.multiplayer_session` is the CTest entry, at a shorter session; `just run-multiplayer`
runs it at the length the pictures are drawn from. Neither needs a display or a graphics device:
there is no renderer anywhere in this artefact's link closure.

## What it is

Two programs, and the split is the same one `samples/08-vertical-slice` uses for the same reason.

| | |
|---|---|
| `cy_sample_multiplayer` | five machines: a host and four clients on one seeded, lossy datagram substrate. It plays the session, records it, replays it, diverges it on purpose and crashes it — and prints `key = value` lines. |
| `multiplayer.py` | the driver. It checks every claim the program printed, paints the two pictures from the program's own trace, and reports through `samples/harness/artefact.py`. |

**A second sample rather than an extension of the vertical slice**, and the proposal says why: the
subject is what happens *between* machines. `samples/08-vertical-slice` stays the single-process
artefact.

## The acts

1. **session** — four clients send their inputs unreliably; ~8 % of datagrams are destroyed,
   duplicated, delayed and reordered by a **seeded** condition simulator. The host simulates behind
   the wall clock by an input delay and **substitutes** the previous input for anything that did not
   arrive; the substituted input is an ordinary command recorded by the ordinary seam. Each client
   predicts its peers, learns from the host's reliable broadcast that it was wrong, and **rolls
   back**. Three claims come out of it: every client converges on the host's state hash, every tick
   is ultimately simulated from authoritative input, and **no explosion plays twice**.
2. **replay** — the host's one log replayed into a fresh world: the same `RecordLog::hash`, the same
   state hash at every tick, and the same again after seeking to a checkpoint.
3. **divergence** — one field of one component on one entity perturbed deliberately, and the report
   the engine produces about it: which tick, which entity, which component, which field, and the
   window that reproduces it.
4. **crash** — the bounded replay buffer flushed into a crash artefact, written, read back, and
   re-simulated to the hash the artefact itself carries.
5. **control** — the negative controls. A perfect network drops nothing and substitutes nothing; two
   runs at one seed produce byte-identical output. A lossy test whose loss cannot be shown to be
   doing anything proves nothing.

## The determinism firewall is armed here

Task 1.4b. M8.c's closing gate found the firewall *armed and guarding nothing*: `guarded_count()`
was zero because the only callers of `declare()` in the tree were test files. This artefact calls
`gameplay::arm_write_firewall()` at session start over a real registry and asserts the startup line,
with `underived` printed beside `guarded` — because a firewall that guards a tenth of the world and
one that guards all of it read identically otherwise.

```
determinism firewall: armed=yes guarded=4/4 (100%) underived=0 [replication=1 persistence=1 by-hand=2 already=2]
```

## What a run says

At 180 ticks, seed `0x0910C0FF`, 40 ms latency with 20 ms jitter, 8 % loss, 2 % duplication and
5 % reordering:

```
3,346 datagrams offered, 276 destroyed, 73 duplicated
95 inputs substituted by the host
271 rollbacks over 1,530 re-simulated ticks across four clients
936 effects offered, 630 suppressed by the ledger, 306 played, 0 played twice
4 of 4 clients converged on the host's state hash
990 records, one log, replayed to the identical RecordLog::hash and the identical state hash
a divergence injected at tick 123 narrowed to entity 4294967298, Health.shield
a 720-record crash ring covering ticks 49 to 179, re-simulated to the hash it carries
```

## The pictures

Both are **diagrams and say so on their own face**: there is no renderer here, so nothing comes off
a device. Every number in them was computed in C++ and written to `session.txt` or `divergence.txt`
before the driver opened them.

* `docs/design/images/m9-multiplayer-session.png` — the session: where the four players are, how far
  back each client had to go tick by tick, and every input the host had to substitute.
* `docs/design/images/m9-divergence-narrowed.png` — the narrowing as the descent it actually is,
  world → archetype → entity → field, with the two hashes and the window.

## The mutation that proves the duplicate-effect check is a check

`docs/ROADMAP.md`: "Rollback re-simulates without re-applying ledgered side effects, **proven by a
duplicate-effect test**." The proof is a control. Replace the body of
`cy::replay::RollbackEngine::offer()` with `return EffectVerdict::Realise;` — the window, the
restore, the cursor and the re-simulation all survive it and only the ledger's decision is gone —
and this program prints a non-zero `session_duplicate_effects` and the driver records a gap, so the
run exits non-zero. The count is the sample's own (`Client::played_`), not the ledger's arithmetic,
which is what makes it a check on the engine rather than on the ledger's bookkeeping.

## With `-D CY_NETWORKING=OFF`

Nothing here is declared: no `cy_sample_multiplayer` and no `integration.multiplayer_session`, and
`just run-multiplayer` reports an absent binary rather than a defect. An option that removes a
subsystem must not turn into a configure error naming the artefact — `CMakeLists.txt` argues it.
