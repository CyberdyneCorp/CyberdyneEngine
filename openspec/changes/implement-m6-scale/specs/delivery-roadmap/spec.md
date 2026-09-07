## ADDED Requirements

### Requirement: An inserted milestone takes a rung, not the end of the ladder
The roadmap SHALL permit a milestone to be inserted between two existing milestones without
renumbering those that follow, so that existing references stay valid. M5.5 was inserted between M5
and M6 on exactly this basis.

An inserted milestone SHALL occupy a **rung between its neighbours** in every mechanism that depends
on milestone order — not a position derived from its absence from a list. The flattened ledger
evaluator inherits "every milestone below this one whose gate is green", so a milestone whose rung is
wrong inherits the wrong set in both directions: it claims criteria that are not below it, and the
milestones that genuinely follow it do not inherit its own.

A milestone identifier that is not on the ladder SHALL be a configuration error reported by
`roadmap-test`, never a value that silently sorts to one end.

#### Scenario: An inserted milestone inherits its predecessors
- **WHEN** a milestone is inserted between two existing ones and a later milestone's ledger is
  evaluated
- **THEN** the later ledger SHALL inherit the inserted milestone's criteria, and the inserted
  milestone's ledger SHALL NOT inherit the later one's

#### Scenario: An unranked milestone is refused
- **WHEN** a ledger names a milestone that the ladder does not contain
- **THEN** `roadmap-test` SHALL fail and name the milestone, rather than the evaluator assigning it a
  position by default
