# Design — M12, The Game

## The defect register

Every defect the slice finds is repaired in a change carrying a **regression test** that fails
without the repair. This table is the register the gate reads: a defect row with no named test is
the state `m12:defects-carry-their-regression-test` refuses.

**A defect found in a row recorded Complete is a finding about the ladder**, not only about the
code — the `Tier when found` column is there so that stays visible rather than being absorbed into
a fix.

| ID | Defect | Row | Tier when found | regression test |
|---|---|---|---|---|
| — | *(the register starts empty and the criterion fails until the slice has found something — a slice that found nothing did not exercise anything)* | — | — | — |

## Why the register is a table and not prose

The gate counts rows and counts tests, and requires the two to be equal. A paragraph saying "we
fixed several issues" is the shape this ladder has refused eleven times: a claim no machine can
check. The register is deliberately mechanical.

## What the slice is scoped to exercise

Each clause drives a row that is currently claimed and unobserved. The slice is finished when those
rows have been exercised together — not when the game is good.

| Clause | Row it drives | What is unobserved today |
|---|---|---|
| 200 units ordered across one map | `navigation` | flow fields claim *"20,000 agents ordered to the same destination"*; 0 of 16 requirements mapped |
| Units drawn at unit count | culling, virtual geometry | instancing and indirect exist; no scene has ever pushed unit counts through them |
| Fog of war, economy, victory rule | `gameplay-framework` | recorded **Complete**, and its spec requires exactly this scenario |
| Two clients, one seed | `simulation-and-determinism` | three named determinism suites do not exist |
| Physics under load | `physics` | fixed-step integration is mapped by nothing, and lockstep desyncs without it |
