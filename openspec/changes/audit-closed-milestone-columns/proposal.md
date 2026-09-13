# Audit the nineteen cells four closed milestones claim, and move or claim each one

## Why

**`m9:record-matches-plan-history` is a declared gap that names nineteen cells, and M10 is the rung
it contracted to close at.** M9's closing gate added `record-matches-plan` — the comparison between a
milestone's own column in `docs/roadmap/capability-matrix.md` and `docs/roadmap/status.yaml` — and
then ran the same comparison over every milestone whose gate was already green. It found nineteen
cells those columns claim that the record does not support, **thirteen of them Complete**:

> M3: `testing-and-quality`; M4: `build-system-and-platforms`; M5: `developer-workflow-and-just`,
> `editor-architecture`, `live-editing`, `project-and-plugins`; M8.b: `asset-import-pipeline`,
> `core-assets-and-io`, `core-memory-and-containers`, `editor-viewport-and-gizmos`,
> `input-and-actions`, `live-editing`, `material-compiler`, `rendering-architecture`,
> `rendering-culling-and-lod`, `rendering-geometry-and-resources`, `shader-system`,
> `swift-scripting`, `thirdparty-dependencies`

It declared the finding rather than claiming the rows, because auditing four earlier columns is work
for a milestone that can examine them rather than for the gate that found them. **M10 tasks.md 6.5 is
that work**, and a gap whose contracted rung passes it by is exactly what the mechanism exists to
prevent: M10's own gate has already run once and reported this criterion failing verbatim.

**How the nineteen got there is worth stating, because it is not carelessness.** Six of M8.b's twelve
Completes were moved *into* M8.b's column by M7's gate, which refused to claim them at M7 — the right
call — and M8.b then neither delivered nor re-read them. M5's gate demoted four rows in the record
and wrote the argument for each, and did not move the matrix cells with them. Nothing compared the
two until M9 wrote the check. **A Complete cell parked in a column nobody audits is how a plan comes
to claim more than a tree contains.**

**And the audit found the other direction, which no gate had looked for.** Four of the nineteen are
rows whose column was right and whose record was never written: `testing-and-quality`,
`build-system-and-platforms`, `developer-workflow-and-just` and `thirdparty-dependencies` were all
recorded at **Seed** from the milestone that seeded them, while the tree has carried them at Working
for several milestones. Each has a column claiming Working at a milestone whose task list never
mentioned it, so no gate ever advanced it and no gate ever refused to. The record was the half that
was wrong.

## What Changes

- **Nineteen cells audited against the tree M10 closes on**, each with the requirement that is unmet
  and the file that shows it. The evidence per cell is
  `docs/roadmap/capability-matrix.md#m10s-record-audit-nineteen-cells-over-four-closed-milestones`.
- **Fifteen cells move.** `editor-architecture` and `live-editing` drop from **W** to **S** in M5's
  column; `project-and-plugins`' **C** leaves M5 and eleven Complete cells leave M8.b, all twelve to
  **M11**; `developer-workflow-and-just`'s **W** moves from M5 to M6, which is the milestone at which
  it became true.
- **Four rows are claimed in the record**, with the rung at which the tier became true verified
  against that rung's own commit: `testing-and-quality` Working at M3, `build-system-and-platforms`
  at M4, `developer-workflow-and-just` at M6, `thirdparty-dependencies` at M8.b.
- **MODIFIED** `delivery-roadmap` → "Implementation status is recorded in one place": the comparison
  SHALL cover every closed column rather than the current one; a cell that exceeds the record SHALL
  be moved or claimed by the gate that finds it rather than deferred a second time; a milestone
  receiving moved cells SHALL have its load restated; and where the record is the half that is
  behind, the correction is to the record, with the rung and the evidence.
- **`m9:record-matches-plan-history` stops being a declared gap.** The criterion stays and now
  passes; `known_gap` and `known_gap_closes` are deleted, because a declared gap that passes fails
  the ledger by design. A latent crash in the same check is repaired in the same edit: `plan.TIERS`
  does not carry `none`, so `tier_rank()` raised for a capability the record had not started, which
  would have fired the first time a closed column planned a cell for an unstarted row.

**No tier in the record moves down, and no milestone's exit criteria change.** Fifteen plan cells
claimed more than the tree holds; the record already said so, and it is the plan that now agrees with
it.

## Impact

- **Specification**: `delivery-roadmap`, one requirement, two new scenarios.
- **Documentation**: `docs/roadmap/capability-matrix.md` — fifteen cells, the Milestone load table
  for M5, M6, M8.b and M11, the "Complete" column for thirteen rows, and a new section carrying the
  evidence for all nineteen. `docs/roadmap/status.yaml` — four rows and the amendment that argues
  them. `docs/roadmap/open-debts.md` regenerates: the "Behind the plan" table, nineteen rows long
  since M9, is now empty.
- **Ledger**: `tools/roadmap/milestones/m9.toml` — one declaration deleted, one guard added.
- **Code**: none.
- **What it costs the plan**: **M11's load goes from 48 to 61.** Thirteen Complete cells moved onto
  the last rung, which already carried "everything remaining". Moving a cell forward is not closing
  it, and on the last rung there is nowhere further to move one — which is the argument for splitting
  M11 that the matrix has been making since M6, now thirteen cells stronger.
