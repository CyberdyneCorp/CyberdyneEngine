## ADDED Requirements

### Requirement: A handover criterion checks entry, not existence, once a split opens several rungs
Every rung of the ladder since M8.a closes by opening the next one, checked by a criterion over
`openspec/changes/**/*<next>*/proposal.md`. The double-star glob is deliberate and SHALL be kept: a
change lives at `openspec/changes/<name>/` while it is open and at
`openspec/changes/archive/<date>-<name>/` once it closes, so a pattern matching only an open change
turns red on the day the next milestone closes, which is the one day the criterion should be most
satisfied.

**Where a split opens several rungs at once, that criterion SHALL check that the next rung has been
ENTERED rather than that its change exists.** A split writes every rung's proposal in one act, so
the existence of the next rung's proposal is guaranteed by the split itself: a criterion asserting it
would pass the moment it was written and could never go red, and **a criterion that cannot fail is
not a criterion**.

The check SHALL use the same glob as the search, so the ladder keeps one shape, and SHALL require
evidence of a deliberate act — at minimum that the next rung's task list records work **begun**,
which is not the same as scoped. A rung's decision-and-spike section can be answered in advance by
the split itself; a checked task in the body of the list cannot. Every rung of one split SHALL use
the same handover check; a ladder whose rungs each invent their own handover has no handover rule at
all.

**The final rung of the ladder SHALL NOT carry an `*-open` criterion, and its absence SHALL be
declared in the ledger rather than left to be read as an omission.** What replaces it is the check
that the ladder ends where it says it does: either the final rung is last everywhere the ladder is
read, or whatever follows it carries a ledger, a gate, a criteria floor and a change directory — all
four, because a ladder extended in three of four places is the defect that has already cost this
project one silent failure per reader.

#### Scenario: A handover criterion could never go red
- **WHEN** a rung's `*-open` criterion asserts only that the next rung's change directory exists, and
  that directory was created by the split that opened both rungs
- **THEN** the criterion SHALL be refused, and replaced by one asserting that the next rung has been
  entered — evidenced by work begun in the body of its task list rather than by its scoping section

#### Scenario: The last rung has no next rung
- **WHEN** the final rung's ledger is written
- **THEN** it SHALL carry no `*-open` criterion, SHALL state in its notes why there is none, and
  SHALL carry a check that fails if the ladder is extended past it in some of the places that read
  the ladder and not others

#### Scenario: One split, one handover shape
- **WHEN** several rungs are opened by one split
- **THEN** every rung of that split SHALL use the same handover criterion shape, decided once in the
  change that performs the split

### Requirement: A superseded milestone change is kept as the record of its own split
Where a milestone is split into rungs, the change that was opened for the unsplit milestone SHALL be
kept rather than deleted, and SHALL point at the rungs that supersede it. It is the record of what
the milestone inherited, what was found about it, and why the split was made — which is evidence the
rungs are built on and which no rung's own proposal restates in full.

The superseded change SHALL NOT be re-scoped into one of the rungs, and SHALL NOT be left claiming
scope the rungs now own. Its task list SHALL record the decision that split it as taken, with the
answer, so that a reader arriving at the unsplit change is sent to the rungs rather than led into a
plan that no longer holds.

#### Scenario: A milestone is split after its change was opened
- **WHEN** a milestone change exists and the milestone is divided into rungs
- **THEN** the original change SHALL remain, SHALL name the rungs, and SHALL record the decision that
  produced them as answered

#### Scenario: The superseded change still claims the work
- **WHEN** a superseded milestone change's scope table still lists capabilities the rungs now own
- **THEN** it SHALL be corrected to point at the rung that owns each, because two documents claiming
  one capability is how a gate comes to be pointed at the wrong one
