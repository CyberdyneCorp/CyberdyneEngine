# simulation-and-determinism Spec Delta

## MODIFIED Requirements

### Requirement: Floating-point policy
The `SamePlatform` profile SHALL require a **controlled floating-point environment** for
authoritative code: a declared rounding mode, denormal handling, and contraction policy. Fast-math
transformations that alter results SHALL be disallowed on authoritative paths.

The `CrossPlatform` and `Lockstep` profiles SHALL require **deterministic math types** for
authoritative computation. These are provided by the optional module defined in
`deterministic-math`: fixed-point scalars, vectors, and angles, and deterministic transcendental
approximations, all computed in integer arithmetic.

A session declaring `CrossPlatform` or `Lockstep` SHALL be accepted only when two conditions hold:

- the deterministic math module is linked;
- every authoritative subsystem in use declares that it meets the profile.

Otherwise it SHALL be refused. The refusal SHALL name the missing module, or the first subsystem,
in stable name order, that still computes authoritative state in floating point.

The engine SHALL NOT claim that arbitrary floating-point code produces identical results across
architectures, compilers, or vector widths. A cross-platform claim SHALL extend only as far as the
architectures whose continuous-integration legs published and agreed.

The deterministic math module SHALL NOT replace the engine's general math library. Rendering,
animation, and effects SHALL continue to use ordinary floating point.

Development builds under deterministic profiles SHALL report two kinds of write to authoritative
state:

- non-finite values written to authoritative fields;
- overflows in deterministic arithmetic on authoritative paths.

A propagated non-finite value destroys reproducibility, and a silent overflow produces a value that
is deterministic but wrong.

#### Scenario: The claim matches the mechanism
- **WHEN** a project requires cross-platform lockstep
- **THEN** its authoritative movement and combat paths SHALL use deterministic math types, and this
  SHALL be a stated cost rather than an assumption

#### Scenario: Presentation keeps floats
- **WHEN** a lockstep session renders
- **THEN** rendering, animation, and effects SHALL use ordinary floating point with no determinism
  requirement

#### Scenario: The refusal names what is still float
- **WHEN** a session declares `Lockstep` with the deterministic math module linked, while abilities
  attributes are authoritative and stored in floating point
- **THEN** configuration SHALL be refused naming the abilities subsystem, rather than refused for a
  missing module

#### Scenario: Without the module nothing cross-platform is accepted
- **WHEN** a session declares `CrossPlatform` in a build that does not link the deterministic math
  module
- **THEN** configuration SHALL be refused with a reason naming the missing module
