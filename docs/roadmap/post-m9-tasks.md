# After M9's gate closes — three things, none of them milestone work

Authorised by the user 2026-09-11. All three are machinery around the work rather than the work, and
none belongs inside a milestone's scope. Do them between M9 closing and M10 launching.

---

## 1. Why continuous integration keeps cancelling

**This is the one that pays for itself**, because `delivery-roadmap` makes CI a precondition for the
audit ever getting shorter:

> **From M9 the audit MAY be reduced** … provided both of these hold, and the reduction SHALL be
> refused if either does not: continuous integration has actually executed, so that cross-platform
> and cross-configuration claims are verified by something other than an agent's reasoning; and the
> permanent gate set covers what the by-hand sweep would otherwise repeat.

Measured 2026-09-11 with `gh run list`:

```
queued      ci  main  schedule  34578492759  5h39m   <- still queued
cancelled   ci  main  push      34551346342  6h41m
cancelled   ci  main  push      34549531188  26m
```

Two consecutive **cancellations** and one run queued for over five and a half hours. So the first
condition does not hold today, the M9 gate should refuse the reduction, and every later milestone
keeps paying eight-hour gates until this is fixed.

**Where to look first, in order:** concurrency groups cancelling in-progress runs when a new push
lands (the two cancellations are both `push`, minutes apart, which is the signature); the 480-minute
timeout on the `milestone` job against a ledger that takes hours; and whether the hosted runner has
the capacity for a job that shape at all. A ledger that cannot finish inside a hosted runner's limits
is an argument for a self-hosted runner, not for a shorter ledger.

**Do not shorten the ledger to fit CI.** The ledger is the thing that has caught eighteen findings.

## 2. The build directory grows without bound

Cleared 2026-09-11: **93 stale directories, 431 GB**, taking the disk from 92 % to 67 %. Verified
first that nothing under `just/`, `tools/`, `.github/` or any milestone ledger names them, and that
only `build/gate` and `build/swift-package` had been touched in the previous two hours.

**The cause is structural and the rule that causes it is a good rule.** Hard rule 11 tells every
agent to build into `build/<label>/`, which is what stopped agents corrupting each other's trees
after three contention incidents. Nothing removes them afterwards, so each milestone leaves five to
ten trees of 8–19 GB. At 171 directories the disk was one milestone from full — and a gate that runs
out of disk mid-ledger fails a milestone for a reason that has nothing to do with the milestone.

**Propose:** a `just build-reap` recipe taking an age threshold, refusing to touch anything modified
inside it, and listing what it will remove before removing it — the shape `maintenance-clean` already
uses. Not automatic on a timer: a tree an agent is still using must never vanish underneath it.

## 3. Virtual geometry has never been photographed

**The most visually demonstrable capability in this engine has no picture, and the reason is a
sequencing accident rather than neglect.**

`virtual-geometry` reached Working at M7 and is real: `samples/07-fidelity` puts **4,478,208 source
triangles in 1,915 clusters across 186 instances** on the device through cluster traversal and the
visibility buffer, with the crack-free check passing 5 of 5 shells over 120 thresholds each, and
22,118,326 interior plus 13,943,657 exterior pixels covered.

All of that is text, because **at M7 the engine could not capture a frame.** The pipeline layer that
turns a rendered frame into a PNG did not exist until M8.c — that is the black-versus-lit pair in the
build log. So `docs/design/images/fidelity-m7.png` is a chart, which was the most its own milestone
could produce.

**Now capturable.** Run the fidelity sample with M8.c's record callbacks and commit:

- the film-detail scene rendered at source resolution — the thing the number describes;
- a **cluster-boundary visualisation**, because "no manual level-of-detail authoring" is the claim
  and cluster colouring is how a person *sees* it rather than reads it;
- ideally the same view at two geometric-error thresholds, since the hierarchy choosing differently
  is the mechanism.

Then add them to both artifacts. Label anything that is not the engine's own output as a diagram,
per the rule M8.c's camera-cut panel already follows.

---

**Order:** 1 first — it is the only one that changes how long every future milestone takes. Then 3,
which is cheap and makes the engine legible to someone who has not read a ledger. Then 2, which is
now urgent again only when the disk climbs.
