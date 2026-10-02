# swift-scripting Spec Delta

## ADDED Requirements

### Requirement: Deterministic math in Swift
`CyberdyneKit` SHALL provide Swift value types for the deterministic math scalar, angle, vector, and
quaternion. Their arithmetic SHALL match the engine's bit for bit:

- wrapping addition and subtraction;
- a full-width product with the engine's rounding rule;
- the engine's division rule, including division by zero.

Transcendental functions SHALL call the engine's ABI entries rather than a Swift reimplementation.

The package's tests SHALL check the Swift arithmetic against the same committed golden vectors the
engine's tests use, so that a divergence between the two languages fails a test.

The determinism rules for gameplay code SHALL state that a fixed step writing authoritative state in
a `CrossPlatform` or `Lockstep` session uses these types.

#### Scenario: Swift and C++ multiply alike
- **WHEN** the package test multiplies every golden-vector input pair with the Swift fixed-point type
- **THEN** every raw result SHALL equal the committed output the engine's suite checks

#### Scenario: Overflow does not trap
- **WHEN** a Swift addition of two fixed-point values overflows
- **THEN** it SHALL wrap as the engine does, rather than trap
