## ADDED Requirements

### Requirement: CY_NETWORKING removes the networking system
`CY_NETWORKING` is declared in `cmake/features.cmake`, defaults OFF, and **gates nothing**: there is
no `if(CY_NETWORKING)` and no `#if defined(CY_NETWORKING)` anywhere in the tree, so the option and
its absence produce identical output. That is the shape M8.b's closing gate found in `CY_UI` — an
option declared, defaulted off, and never once compared against itself — and M8.c had to repair it.

The networking system SHALL be removable at build time via `CY_NETWORKING`, and the engine SHALL
build and pass its test suites with the option ON and with it OFF. Neither direction may be assumed.

#### Scenario: The option is off
- **WHEN** the engine is built with `-D CY_NETWORKING=OFF`
- **THEN** `src/networking/` SHALL be absent from the build, no networking suite SHALL be declared,
  and every other suite SHALL pass

#### Scenario: The option is on
- **WHEN** the engine is built with `CY_NETWORKING` at its default
- **THEN** the networking system SHALL be present and its suites SHALL be declared and pass
