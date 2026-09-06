## MODIFIED Requirements

### Requirement: Milestone gates do not regress
Once a milestone's criteria are green, its checks SHALL join the permanent continuous integration
set and SHALL remain green for every subsequent change.

A later milestone SHALL NOT be permitted to break an earlier milestone's gate. Where a change
genuinely supersedes an earlier criterion — a sample replaced, a format changed — the supersession
SHALL be recorded in a change, and the replacement check SHALL land in the same change.

**The permanent set is flat and deduplicated, and a milestone's ledger SHALL NOT re-run an earlier
milestone's ledger.** A ledger SHALL evaluate two things: the permanent gate set, once, with each
distinct criterion run a single time however many milestones declared it; and its own new criteria.

Chaining ledgers — each milestone's first criterion running the previous milestone's, which runs the
one before it — is the obvious implementation and it compounds badly. Measured across the first five
milestones: 123 criterion invocations over 89 distinct criteria, 34 of them redundant, with
`four-profiles` — a full four-configuration build and test — declared by four ledgers and therefore
executed four times by a single run of the newest. At twelve milestones the chain is twelve deep.

The cost is not only time. Re-running one criterion many times multiplies its failure probability by
the same factor, so a single marginal test becomes a flake that fails the build for reasons
unrelated to the change under test. That has already happened in this project: a unit case sitting
on its time budget produced roughly a dozen exposures per pull request purely through ledger
nesting, and cost a full diagnosis cycle before the multiplication was recognised as the cause.

Deduplication is therefore a correctness property, not an optimisation.

#### Scenario: A criterion declared by several milestones runs once
- **WHEN** the newest milestone's ledger is run
- **THEN** each distinct criterion in the permanent set SHALL execute exactly once, however many
  milestones declared it

#### Scenario: Ledger cost does not grow with the ladder
- **WHEN** a twelfth milestone is added
- **THEN** its ledger SHALL evaluate the permanent set plus its own criteria, and SHALL NOT be
  deeper than a ledger from earlier in the ladder

#### Scenario: A later milestone breaks an earlier sample
- **WHEN** work in M7 breaks the M4 character controller sample
- **THEN** the merge SHALL be blocked until the sample runs again or its replacement lands

#### Scenario: A gate is retired deliberately
- **WHEN** a milestone criterion is superseded
- **THEN** an OpenSpec change SHALL record why, and the new check SHALL be in the same change
