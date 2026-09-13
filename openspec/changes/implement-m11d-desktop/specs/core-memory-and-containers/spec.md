## ADDED Requirements

### Requirement: Every attribution axis has a producer
`Memory diagnostics` requires reporting attributable by domain, by type, by thread, by world cell and
by asset, *"so that 'why is this region consuming this much' is answerable"*. The mechanism exists
and nothing pushes it: the only files naming the attribution scope are its own header, its
implementation, its test and its README, so four of the five axes are never populated in a running
engine and a report answers the question with an empty table.

A diagnostic with no producer is not a diagnostic. The engine SHALL therefore require, for each axis
this specification names, that **the module owning the identity pushes it**: the cell activation path
for the world-cell axis, the asset load path for the asset axis, and the device-memory allocator for
the `GPU` domain. The thread axis is captured rather than declared, because asking a caller for it is
asking it to get it wrong.

A report SHALL remain able to say what it could not attribute — unreported and unattributed bytes
reconciling against live bytes — so that "nobody declared this axis" is distinguishable from "the
table was too small". That distinction SHALL NOT be used as a substitute for a producer.

Adoption SHALL be checkable rather than assumed: the engine SHALL be able to demonstrate, in a test
that exercises a real workload, that a report carries non-empty rows for each axis a running engine
can populate.

#### Scenario: A real workload attributes along every axis it can
- **WHEN** a world activates cells, loads assets and allocates device memory
- **THEN** a memory report SHALL carry rows attributed to a world cell, to an asset and to the `GPU`
  domain, rather than reporting every byte unattributed

#### Scenario: An axis with no producer is visible as such
- **WHEN** an axis this specification names is populated by no module
- **THEN** that SHALL be reported as unattributed rather than presented as a complete report, and it
  SHALL be recorded as an open gap with the rung that closes it

#### Scenario: Device memory reaches the GPU domain
- **WHEN** a graphics backend allocates device memory
- **THEN** the bytes SHALL appear under the `GPU` domain's sub-domains in the same report as CPU
  memory, so the budget tree reflects what the device holds
