## ADDED Requirements

### Requirement: A spike may resize a milestone as well as redirect it
A risk spike's finding SHALL be allowed to change a milestone's **size**, not only its design. Where
a spike establishes that the planned approach is unavailable and the available one is materially more
work, the milestone SHALL be re-scoped before it is built rather than after it fails to close.

This is the companion to the rule that a milestone blocked by its own spike contains two. That rule
catches a settled half waiting on a risky one; this one catches a milestone that was sized against an
approach the spike then removed. Both are discovered at the same moment — when the spike reports —
and both are cheaper to act on then than at the closing gate.

Re-scoping SHALL move whole capabilities, with their exit criteria, and SHALL NOT narrow a capability
in place. A criterion whose subject has been deferred SHALL move with its subject; leaving it behind
produces a check with nothing to check.

#### Scenario: A spike finds the approach costlier than planned
- **WHEN** a spike establishes that the planned approach cannot be taken and the available one is
  materially larger
- **THEN** the milestone SHALL be re-scoped before implementation begins, and the deferred scope
  SHALL be recorded on the ladder with its own artefact

#### Scenario: A criterion follows its subject
- **WHEN** a capability is deferred out of a milestone
- **THEN** every exit criterion whose subject is that capability SHALL move with it, rather than
  remaining as a criterion the milestone can satisfy vacuously
