# Spec Delta

## ADDED Requirements

### Requirement: Integrated visual clouds execute on the graphics device
The rendered world demonstration SHALL evaluate its visual cloud contribution on the graphics
device. The seeded, layered dome-vertex density march SHALL consume the same time, weather, and
viewing inputs as its CPU reference and its output SHALL be used by the rendered sky. It does not
replace the atmosphere module's full per-pixel spherical-shell march.

#### Scenario: A rendered sky contains clouds
- **WHEN** the world renders a frame whose weather has nonzero cloud coverage
- **THEN** the command stream SHALL contain the cloud device workload and the sky SHALL consume its output

#### Scenario: Cloud workload is absent
- **WHEN** the cloud dispatch is removed or its output is disconnected from the sky
- **THEN** the device criterion SHALL fail
