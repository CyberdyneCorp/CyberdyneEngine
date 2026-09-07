# `src/servers/residency/` — the shared residency policy

Layer 2. `cy::servers-residency`. M6 tasks 4.1 to 4.3, capability `residency` at **Working**.

Every paged subsystem in this engine independently invents the same vocabulary — request, priority,
residency, budget, eviction, prefetch. `residency` says that is one thing, and this module is it:

> **shared policy, separate storage.**

## What it owns

| | |
|---|---|
| `types.h` | `Subsystem`, `PageKey`, `CostClass`, `QualityReason` — the vocabulary, and nothing that could hold a texel |
| `importance.h` | One render importance per instance, and a **declared transform** per consumer |
| `request.h` | The one scoring function, and the queue that deduplicates before it runs |
| `deadline.h` | One prediction becomes one deadline per consumer, plus prediction accuracy |
| `policy.h` | Budgets, the six reduction levers, the eviction score, the churn tracker |
| `server.h` | `ResidencyServer` — the frame, the holds, the pressure response, the diagnostics |

## What it does not own, and cannot

Page storage, page formats, page production. `residency` states that a change moving storage into
this layer violates the requirement, so the module is built so that it could not: a page is a
`Subsystem` tag and an **opaque 56-bit number**, there is no interface through which a page's
contents could arrive, and the dependency list is `cy::core-base` and `cy::core-memory` and nothing
else. Virtual texturing depends on this module; this module knows nothing about virtual texturing.

## The frame

```
  request(...)          many times, from every subsystem, deduplicated on the way in
  schedule(options, s)  the policy decides: these may be brought in, those must go
  ... the subsystem fetches, decodes, renders or composes, in its own storage ...
  note_resident(...)    it reports what arrived
  note_released(...)    it reports what it let go
  end_frame(now)        ages the records, prunes churn, resolves deadlines
```

An admission spends the budget at the moment it is issued, not at the moment the bytes arrive: the
record is created `pending` and flipped by `note_resident`. Without that, a frame admitting five
pages into a three-page budget sees an empty cache at every decision and admits all five. The
subsystem hands an admission it cannot act on back with `cancel_admission`, and anything still
pending after `kPendingExpiryFrames` is reclaimed and counted as `abandoned_admissions`.

## Residency and activation are two facts (M6 exit criterion)

**A test holds bytes resident with simulation off** — `tests/test_separation.cpp`.

`hold()` takes a page and a reason. It does not take, mention or consult an activation state, a
world, a tick or a simulation, and there is no simulation clock anywhere in `server.h`.
`set_active()` records what the owning subsystem activated; it is an input to eviction *preference*
and to diagnostics, and never a precondition for residency. The test asserts both directions, and
the control — releasing the **hold**, not the activation, is what makes a page evictable.

design.md §5 lists this among the invariants that cannot be retrofitted: every later system that
streams assumes it.

## Teardown is not a shutdown path

M6 creates and destroys worlds continuously. `unregister_subsystem()` and `reset()` are defined
*mid-flight*: they drop the records for outstanding pages, invalidate every hold on them, and leave
the accounting at zero. The class is internally synchronised — completions arrive from IO and
production workers — and `~ResidencyServer` takes the lock so a call already in progress finishes
before the tables go. `tests/test_teardown.cpp` does that under four concurrent workers, two hundred
times, and destroys two hundred servers still holding pages, holds, requests and deadlines.

## Tests

    just test-unit residency                # the policy, and the separation criterion
    just test-integration residency_teardown  # teardown under load

Not gated by any option: the module compiles in every build, so both suites run in every
configuration.
