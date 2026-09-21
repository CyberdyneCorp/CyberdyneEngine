## ADDED Requirements

### Requirement: A deferral names a rung, not a date
A capability that reaches 1.0 without a Complete cell SHALL record its re-entry as a **rung on the
ladder**, and that rung SHALL carry a ledger, a gate, a floor and a change directory. A re-entry
recorded as a date, as "after 1.0", or as a milestone that does not exist SHALL NOT satisfy this
requirement.

The distinction is the whole of the rule: a re-entry named in the record is a decision somebody
can act on, and one named nowhere is an oversight wearing the same clothes. The project has already
produced the weaker form — four release recipes refused naming *"M12 — build-and-packaging"* when
there was no M12, and a declared gap pointed at M11.a for two rungs after M11.a had closed, which
is the one shape a deadline cannot survive.

#### Scenario: A capability is deferred past 1.0
- **WHEN** the 1.0 record marks a capability deferred rather than Complete
- **THEN** the deferral names a rung that exists on the ladder with all four attachments
- **AND** a check fails if the named rung is absent from `record.MILESTONES`, from `gates.toml`,
  from `tools/roadmap/milestones/`, or from `openspec/changes/`

#### Scenario: A deferral names a rung that has already closed
- **WHEN** the rung a deferral names has its gate recorded green
- **THEN** the ledger fails naming the deferral and the rung
- **AND** the deferral is re-pointed by a deliberate act rather than by the gate that found it

### Requirement: M12 carries the three capabilities 1.0 ships without
`xr-support`, `ml-inference` and Android SHALL be deferred to M12 by name. Each SHALL reach
Complete there, or be deferred again through a change that argues for it and names a further
re-entry.

M12 SHALL NOT be used as a destination for work a current rung finds inconvenient. A fourth
capability arrives only through a change that argues for it; the ladder already carries the cost of
one rung that became a collector of everything nobody did.

#### Scenario: A rung tries to move work to M12 without a change
- **WHEN** a criterion or task re-points a capability at M12 outside a change proposing it
- **THEN** the ledger fails, naming the capability and the rung that moved it
