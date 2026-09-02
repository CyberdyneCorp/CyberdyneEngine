## MODIFIED Requirements

### Requirement: Thread roles
The engine SHALL define these thread roles and their ownership:

| Thread | Owns / may touch |
|---|---|
| Main | Platform event pump, window and input, the frame schedule, editor UI |
| Simulation | ECS world mutation outside parallel system execution, deferred command flush |
| Job workers | Parallel system execution, culling, animation sampling, asset decode, physics jobs, acoustic simulation |
| Render | Render graph recording and GPU submission; owns all RHI objects |
| Audio (realtime) | Audio mixing and effect processing; owns playback state; hard-realtime, never blocks |
| Asset I/O | File reads, decompression, streaming; never touches ECS or GPU objects |

Where the platform requires it (macOS, Windows message pumps), the main and simulation roles MAY
share one OS thread; the ownership rules still apply.

Work whose results the realtime audio thread consumes — acoustic simulation in particular — SHALL
run on job workers and publish through a double-buffered store. The realtime audio thread SHALL
NOT wait on job workers, and job workers SHALL NOT block on the audio thread.

#### Scenario: RHI object touched off the render thread
- **WHEN** code on a worker thread attempts to record RHI commands
- **THEN** a development-build assertion SHALL fire naming the violated thread role

#### Scenario: Asset thread hands off safely
- **WHEN** the asset I/O thread finishes loading
- **THEN** it SHALL publish the result through a queue consumed on the simulation thread; it
  SHALL NOT insert into the ECS world directly

#### Scenario: Realtime audio never waits
- **WHEN** acoustic simulation on job workers has not completed a new result
- **THEN** the audio callback SHALL use the previously published result and meet its deadline
