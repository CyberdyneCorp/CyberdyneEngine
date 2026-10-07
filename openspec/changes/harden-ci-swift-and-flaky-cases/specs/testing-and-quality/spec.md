## ADDED Requirements

### Requirement: The first case is not charged for the process's first touches
Before any case runs, the test harness SHALL page in the code a case runs that the loader has not:
the test executable and the C math library. A case's CPU budget SHALL measure the case, not the
first use of code every process loads.

#### Scenario: The first case to call libm
- **WHEN** the first case in a binary to call `acosh`, `expm1` or `cbrt` runs
- **THEN** every page of the C math library's code SHALL already be mapped, and the case SHALL not
  take a fault for it
