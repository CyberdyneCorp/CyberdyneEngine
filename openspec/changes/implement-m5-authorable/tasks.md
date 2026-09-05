# Tasks: M5 — Authorable

Ordered. The live-bridge spike first. Then the carried-forward debts, then the editor.

**Sections 2 onward are authored after M4's gate reports**, because the editor is built against
M4's real ABI and overlay rather than a prediction of them. Section 1 is written now because its
items are known and one of them gets more expensive every milestone it waits.

## 0. Spike — live bridge latency

M5's named risk. Its only deliverable is a decision.

- [ ] 0.1 Measure gizmo-drag round-trip latency over the out-of-process boundary, before any panel
      is built
- [ ] 0.2 Measure it again for the case where the runtime is not local
- [ ] 0.3 If interactive manipulation cannot feel local, propose the change before section 2 —
      the honest alternative is in-process hosting as the local default with out-of-process as the
      remote path, which the specification already permits

## 1. Carried forward

### 1.1 The flattened milestone ledger — **do this first**

The cost compounds with every milestone that passes without it, and it has already manufactured one
failure.

- [ ] 1.1.1 A ledger evaluates the permanent gate set **once, deduplicated**, plus its own new
      criteria — and does not invoke another milestone's ledger
- [ ] 1.1.2 Remove the `m<n>-green` chaining criteria from m1 through m4
- [ ] 1.1.3 Prove the deduplication: `four-profiles` executes **once** in a full run of the newest
      ledger, against four times today
- [ ] 1.1.4 Confirm the ladder is still enforced — a regression in an M0 criterion must still fail
      the newest milestone's ledger, because the criterion is in the permanent set
- [ ] 1.1.5 Record the before and after invocation counts

### 1.2 Stale feature annotations

- [ ] 1.2.1 `cmake/features.cmake` — `CY_VIRTUAL_GEOMETRY` says M10 and the roadmap says M7;
      `CY_UI` says M9 and the roadmap says M8. Audit every annotation against
      `docs/roadmap/capability-matrix.md` and correct them.

### 1.3 From M4's gate

- [ ] 1.3.1 *(authored when the M4 gate reports)*

## 2. The editor, the bridge, and the artefact

*Authored after M4's gate reports, against the real ABI and overlay handoff.*
