## ADDED Requirements

### Requirement: The 1.0 record is a criterion, not a document
The 1.0 gate's claim is that **every capability is Complete or explicitly deferred**. That claim
SHALL be evaluated by a check that reads the status record and the deferral register and **fails
naming any capability that is neither**. It SHALL NOT be satisfied by a document asserting it.

This rule exists because the project has already shipped the weaker version once. Four capabilities'
Working tier was claimed by a plan column and evaluated by **no criterion in any ledger**: the only
`expect_tiers` entry naming any of them expected `seed`, and an exit tier is treated as a **floor**,
so the recorded tier could never contradict the claim. The gap was closed by writing the tiers into
the record, and the next gate reopened it, because **a tier written into a file that nothing
re-checks is not a tier that was checked**.

The 1.0 record SHALL additionally state **what 1.0 is and what it is not**: which capabilities are
Complete, which are deferred, and what a person adopting the engine should and should not expect of
it. A record that lists only what was achieved is a record that cannot be used to make a decision.

**Only the final rung of the ladder SHALL record a new deferral.** A rung before it that cannot
finish a capability demotes it, with the reason, to a later rung; a deferral is the final rung's act
because it is the point at which "later" stops being available. This is what makes the deferral
discipline enforceable rather than a habit.

#### Scenario: A capability is neither Complete nor deferred
- **WHEN** the 1.0 check is evaluated and a capability in the record is below Complete with no entry
  in the deferral register
- **THEN** the check SHALL fail naming that capability and its recorded tier

#### Scenario: A Complete tier no criterion evaluates
- **WHEN** a capability is recorded Complete and no criterion in any ledger evaluates it
- **THEN** the check SHALL fail naming the capability, because an unevaluated tier is a claim rather
  than a record

#### Scenario: A deferral is recorded before the final rung
- **WHEN** a rung that is not the final one records a new deferral
- **THEN** it SHALL be refused, and the capability SHALL be demoted to a later rung with its reason
  instead

### Requirement: A recipe that refuses names a rung that exists
Where a recipe, a build step or a tool refuses because the work is not yet scheduled, the milestone
it names SHALL be **on the ladder** and SHALL **not yet have closed**. A refusal naming a milestone
that does not exist, or one that closed several rungs ago, is a stale statement of the plan presented
to a user as the current one.

This SHALL be a check over the recipe set and the ladder, not a review. The project has already
found and corrected one instance by hand, in one file, leaving the same defect live in others — which
is precisely the shape that a check replaces and a review does not.

The check SHALL name the recipe, the milestone it refuses with, and whether that milestone is absent
from the ladder or already closed.

#### Scenario: A refusal names a milestone that is not on the ladder
- **WHEN** a recipe refuses naming a milestone identifier the ladder does not contain
- **THEN** the check SHALL fail naming the recipe and the identifier

#### Scenario: A refusal outlives its milestone
- **WHEN** a recipe refuses naming a milestone whose gate is green
- **THEN** the check SHALL fail, because the capability it was waiting for has landed and the
  refusal is now false

#### Scenario: A refusal is legitimate
- **WHEN** a recipe refuses naming a rung on the ladder that has not yet closed
- **THEN** the check SHALL pass, and the refusal SHALL state the capability the work belongs to

## MODIFIED Requirements

### Requirement: Deferred scope has re-entry points
Scope that the specifications defer SHALL be recorded in the roadmap with the milestone at which it
would be reconsidered, the seams that must remain open until then, and the check that proves those
seams are still open.

Deferred scope SHALL NOT be silently dropped, and SHALL NOT be silently started. Bringing deferred
scope forward SHALL be an OpenSpec change.

At minimum the following SHALL be recorded as deferred with re-entry points: XR, fluid simulation,
hair and fibre rendering, offline simulation import, voxel rendering, authority migration,
distributed build execution, cloud save backends, and optional audio middleware.

**A deferral SHALL carry three things, and one carrying fewer SHALL be refused:**

1. **What is unmet** — read requirement by requirement against the tree at the gate, not against the
   scope statement written before the work began.
2. **Why it is deferred** — a measurement or a decision, named. Exhausting a milestone is a schedule
   and not a reason; a dependency adoption that must go through its own change flow is a reason.
3. **The condition that brings it back** — a state of the world rather than a date.

**A capability demoted from a rung SHALL arrive at the next one carrying the reason that demoted
it**, and a capability demoted **twice** SHALL be treated as **mis-scoped rather than late**: it SHALL
be re-scoped through a change against its own specification before it is recorded either way. This
rule was written in advance of the case that proved it and the case arrived on schedule.

**A seam check that reports green over a seam that is only half open SHALL be corrected rather than
relied upon.** A deferral whose protection is a check that cannot see half of what it protects is a
deferral with no protection, and it is worse than an unchecked one because it reads as safe.

#### Scenario: A deferred seam is protected
- **WHEN** a render change would break multi-view rendering
- **THEN** the XR prerequisite check SHALL fail, and the change SHALL either preserve the seam or
  propose closing it explicitly

#### Scenario: Deferred work is not accidental scope
- **WHEN** a contributor begins implementing a deferred capability
- **THEN** a change SHALL first move it onto the ladder, with its dependencies analysed

#### Scenario: A deferral is missing one of its three parts
- **WHEN** a deferral is recorded without what is unmet, without a named reason, or without a
  condition that brings it back
- **THEN** it SHALL be refused, and the capability SHALL be finished rather than recorded

#### Scenario: A capability is demoted a second time
- **WHEN** a capability's Complete cell is moved for the second time
- **THEN** it SHALL be treated as a finding about the plan, and re-scoped through a change against
  its own specification rather than moved again

#### Scenario: A seam check covers only part of its seam
- **WHEN** a prerequisite check passes while a documented half of the prerequisite is known not to
  hold
- **THEN** the partial coverage SHALL be recorded against the deferral, and closing or completing
  that half SHALL be a deliberate act before 1.0 is recorded
