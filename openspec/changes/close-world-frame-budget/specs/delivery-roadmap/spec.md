# Spec Delta

## ADDED Requirements

### Requirement: The integrated world holds its shipping frame budget on a graphics device
The fixed 64-frame M10 world take at 960x540 in a Shipping build SHALL have a worst measured frame
time at or below 16.7 ms on a declared real graphics device. The measured path SHALL dispatch the
terrain substrate, visual cloud composition, and visual foam workloads and SHALL render their results.

The evidence SHALL name the backend, adapter, operating system, build configuration, seed, frame
count, resolution, mean, best, and worst frame times. A missing device, missing workload dispatch,
missing output frame, or frame above the threshold SHALL fail the criterion.

#### Scenario: Device take holds budget
- **WHEN** the fixed take completes on a real graphics device with all three visual workloads dispatched
- **THEN** the criterion SHALL pass only when every frame is at or below 16.7 ms

#### Scenario: A required dispatch is bypassed
- **WHEN** any required visual workload executes on the CPU or is omitted from the measured frame
- **THEN** the criterion SHALL fail even if the reported frame time is below 16.7 ms

### Requirement: The integrated world's headless budget measures authoritative simulation
The headless M10 world take SHALL measure authoritative CPU simulation and SHALL exclude visual-only
terrain substrate, visual cloud composition, and foam preparation that a rendered run performs on a graphics
device. It SHALL use the same seed, simulation ticks, and generated authoritative state as the device
take and SHALL remain within 16.7 ms per frame.

#### Scenario: Headless take holds its simulation budget
- **WHEN** the fixed take runs without a graphics device
- **THEN** it SHALL produce the expected authoritative digest and pass only when every simulation frame is at or below 16.7 ms

#### Scenario: Visual work leaks into headless timing
- **WHEN** a visual-only producer is reintroduced into the timed headless frame
- **THEN** the headless criterion SHALL fail its workload audit
