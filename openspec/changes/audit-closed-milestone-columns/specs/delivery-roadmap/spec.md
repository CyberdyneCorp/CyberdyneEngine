## MODIFIED Requirements

### Requirement: Implementation status is recorded in one place
The project SHALL maintain exactly one authoritative record of per-capability implementation
status: the capability, its tier, the milestone that last advanced it, and the change that did so.

A change that implements or advances a capability SHALL update that record in the same change. A
change SHALL NOT claim a tier the record does not support.

Status SHALL be reported by a recipe rather than read by hand, and the recipe SHALL fail when the
record references a capability that does not exist, or omits a capability that does.

**The comparison between the plan and the record SHALL cover every closed milestone's column, not
only the current one.** A milestone's closing gate SHALL compare the record with the column of every
milestone whose gate is already green, and SHALL treat a cell that exceeds the record as a finding of
that gate.

A cell that exceeds the record SHALL be **moved or claimed** — moved to the milestone at which its
unmet requirement is scheduled, or claimed in the record with the rung at which the tier became true
— and it SHALL NOT be left standing in a closed column. Moving a cell is moving a claim into a column
that has not been audited yet, so a milestone that receives moved cells SHALL have its load restated
in the same change.

**Where the record is behind the plan rather than the plan ahead of the record, the correction is to
the record.** A capability whose column claims a tier the tree supports, and which no milestone's
task list ever named, is a row no gate advanced and no gate refused; the audit that finds it SHALL
record the tier and the rung at which it became true, with the evidence, rather than move the cell.

#### Scenario: Status cannot drift from the specifications
- **WHEN** a capability is added or renamed
- **THEN** the status recipe SHALL fail until the record is updated

#### Scenario: A tier claim is traceable
- **WHEN** a capability is listed as Working
- **THEN** the record SHALL name the change that advanced it

#### Scenario: A parked cell is found by the gate that closes a later milestone
- **WHEN** a closed milestone's column claims a tier the record does not support
- **THEN** the closing gate SHALL report it with the capability, the planned tier and the recorded
  tier, and the cell SHALL be moved or claimed rather than deferred a second time

#### Scenario: The record is the half that is wrong
- **WHEN** a column claims a tier the tree supports and the record holds a lower one
- **THEN** the record SHALL be corrected, naming the milestone at which the tier became true and the
  evidence for it, rather than the column being moved to match the record
