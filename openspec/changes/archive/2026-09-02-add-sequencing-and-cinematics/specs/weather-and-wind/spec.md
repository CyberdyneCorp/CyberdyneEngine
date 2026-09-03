## MODIFIED Requirements

### Requirement: Weather presets and transitions
Weather SHALL be authorable as **presets** describing a target environmental state — clear, overcast,
light rain, storm, snowstorm, sandstorm, and project-defined — not merely a visual configuration.

Applying a preset SHALL **transition** toward it rather than switching, with **per-property
durations**: cloud coverage over minutes, precipitation over a shorter period, wind over another.

Transitions SHALL be deterministic where the session's determinism profile requires it, and
schedulable in advance so that a designed weather sequence is reproducible.

`sequencing-and-cinematics` drives weather by **setting this state** through environment tracks —
target preset, transition timing, and individual parameters — and a separate cinematic-only weather
implementation SHALL NOT exist. A weather change authored on a timeline and one triggered by
gameplay SHALL be the same operation.

#### Scenario: Weather arrives, it does not appear
- **WHEN** a storm preset is applied
- **THEN** cloud coverage, wind, and rain SHALL transition on their own time constants

#### Scenario: Cinematics use the same system
- **WHEN** a sequence drives the weather
- **THEN** it SHALL set the same state, not a parallel visual path

#### Scenario: Authored and gameplay weather agree
- **WHEN** the same storm is triggered by a sequence in one mission and by gameplay in another
- **THEN** both SHALL produce the same environmental state through the same transition mechanism
