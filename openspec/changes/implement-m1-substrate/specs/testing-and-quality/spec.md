## MODIFIED Requirements

### Requirement: Memory and concurrency correctness
CI SHALL run, at least nightly: AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer
builds over the unit and integration suites.

The engine SHALL report allocation leaks at shutdown in development builds, and leaks SHALL fail
the test run.

The sanitizer gate SHALL name **which suites run under which sanitizer**, and that set SHALL be
executed by continuous integration rather than merely made available as a build option. A sanitizer
that is wired but never run is not a gate.

A subsystem that **intentionally holds an allocation for the life of the process** — a pooled ring,
a registry, an interned table — SHALL declare it where the leak detector can see the declaration, so
that a report is either a real defect or a declared exception, and never a standing failure the
suite is expected to tolerate.

#### Scenario: A wired sanitizer is actually run
- **WHEN** a sanitizer is enabled by a build option
- **THEN** a named suite SHALL run under it in continuous integration, or the option SHALL be
  documented as developer-only and excluded from the gate set

#### Scenario: Intentional lifetime allocation is declared
- **WHEN** a subsystem pools memory for the life of the process
- **THEN** the leak detector SHALL be told, and the suite SHALL be green rather than expected-red

#### Scenario: Data race
- **WHEN** a system writes a component it did not declare
- **THEN** either the access assertion or ThreadSanitizer SHALL catch it

#### Scenario: Leak in a test
- **WHEN** a test leaves allocations outstanding
- **THEN** the run SHALL fail with the tag and call site
