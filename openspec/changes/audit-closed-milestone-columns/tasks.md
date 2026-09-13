# Tasks: audit the closed milestones' columns

- [x] 1.1 Audit all nineteen cells against the tree M10 closes on, naming for each the requirement
      that is unmet and the file that shows it — not against the gate that parked the cell, because
      two of the nineteen had moved since (`live-editing` gained the other half of its bridge at M7,
      `editor-viewport-and-gizmos` gained engine-side picking at M8.a)
- [x] 1.2 Move the fifteen cells the code does not support, in `docs/roadmap/capability-matrix.md`,
      and restate the Milestone load table for every column that changed — M5, M6, M8.b and M11
- [x] 1.3 Claim the four rows whose column was right and whose record was never written, each with
      the rung at which the tier became true verified against that rung's own commit
- [x] 1.4 Write the evidence for all nineteen into the matrix, in the shape every closing gate since
      M5 has used, and argue the four claimed rows in `docs/roadmap/status.yaml`'s own amendment
- [x] 1.5 Delete the `known_gap` declaration from `m9:record-matches-plan-history`, which now passes,
      and guard the `tier_rank()` crash the same check carries for an unstarted capability
- [x] 1.6 **Prove the check can still fail**: restore one moved Complete cell, lower one claimed row
      in the record, and plan a closed column for an unstarted capability — three mutations, three
      reds, each restored
- [x] 1.7 MODIFY `delivery-roadmap` so the next parked cell is found by the gate that closes the
      next milestone rather than by the gate after it
- [ ] 1.8 Archive with the change that closes M10, so the specification and the status record move
      together
