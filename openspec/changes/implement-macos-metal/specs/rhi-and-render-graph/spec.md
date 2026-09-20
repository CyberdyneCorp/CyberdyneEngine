## ADDED Requirements

### Requirement: Native Metal hardware conformance evidence
Metal conformance results SHALL identify the tested device, Apple GPU family support, and argument
buffer tier. Compilation and device discovery SHALL NOT count as rendering, bindless, memoryless
attachment, or golden-image conformance.

#### Scenario: Tier 2 hardware is available
- **WHEN** the native conformance suite runs on an Apple GPU reporting Tier 2 argument buffers
- **THEN** it SHALL exercise the descriptor path with shader-visible results and report those results
- **AND** an unexecuted path SHALL remain explicitly not evaluated

#### Scenario: The device seed compiles
- **WHEN** only the Metal seed and its format assertions have compiled
- **THEN** the report SHALL state compilation evidence without claiming a functioning engine backend
