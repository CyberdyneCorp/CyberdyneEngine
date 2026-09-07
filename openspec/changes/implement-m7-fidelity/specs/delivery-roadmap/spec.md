## ADDED Requirements

### Requirement: The tier plan obeys the dependency rules, and a tool says so
A capability SHALL NOT be planned to reach **Complete** at a milestone earlier than the one at which
every prerequisite its specification names reaches **Working**, in the same way it SHALL NOT be
planned to reach Working before its prerequisites reach Seed. Both halves of that rule SHALL be
**checkable by a tool** rather than by review.

The check SHALL read the planned tiers from `docs/roadmap/capability-matrix.md` and the dependency
order from the specifications, and SHALL fail naming the capability, the prerequisite, and the two
milestones — so a plan that violates the rule is a build failure at the moment it is written rather
than a discovery at the gate that has to close it.

M6 is the evidence that review does not catch this. Two rows of its plan violated the rule and both
were found by hand at its closing gate: `rendering-geometry-and-resources` was planned Complete at M6
while its Skinning requirement reads the GPU pose world, which is `animation-and-skinning` at M8; and
`asset-import-pipeline` was planned Complete at M6 while its Virtual geometry cooking requirement
depends on `virtual-geometry`, at M7.

#### Scenario: A plan that completes before its prerequisite
- **WHEN** the matrix marks a capability Complete at a milestone earlier than one at which a
  prerequisite its specification names reaches Working
- **THEN** the check SHALL fail, naming the capability, the prerequisite and both milestones

#### Scenario: A deliberate exception
- **WHEN** the scope that depends on the prerequisite is deliberately excluded from that capability
- **THEN** the exclusion SHALL be recorded in the deferrals register and the check SHALL read it,
  rather than the rule being relaxed

### Requirement: The plan documents agree with each other
`docs/ROADMAP.md`'s per-milestone work table, the milestone column of
`docs/roadmap/capability-matrix.md`, that document's own milestone-load summary, and the milestone
ledger's expected tiers SHALL describe the same set of capabilities at the same tiers for a given
milestone, and the agreement SHALL be checked by a tool.

Three of the four disagreed about M5's scope — nine capabilities, sixteen, fourteen and fourteen —
and the disagreement was found by reading rather than by running. It recurred at M6: the roadmap's
M6 table listed eight capabilities where the matrix column listed eleven.

#### Scenario: The four sources disagree
- **WHEN** a milestone's work table, matrix column, load summary and ledger tiers do not name the
  same capabilities at the same tiers
- **THEN** the check SHALL fail, naming each disagreement and the two documents that hold it
