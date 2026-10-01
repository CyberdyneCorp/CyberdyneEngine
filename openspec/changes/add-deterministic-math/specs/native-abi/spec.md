# native-abi Spec Delta

## ADDED Requirements

### Requirement: Deterministic math values at the boundary
Deterministic math values SHALL cross the ABI as their raw integer representations:

- a fixed-point scalar as a signed 64-bit integer;
- an angle as an unsigned 32-bit integer;
- vectors and quaternions as plain structs of those.

The dynamic value type SHALL gain a tag for the fixed-point scalar, using the 64-bit integer
payload, so that its tag names its storage format.

The ABI SHALL expose the deterministic transcendental functions as entries, including variants that
operate on caller-supplied spans. A module in another language SHALL then call the engine's
implementation rather than carry a second one.

These additions SHALL be appended in one minor version without changing any existing entry.

In a session whose profile is `CrossPlatform` or `Lockstep`, a floating-point setter SHALL refuse to
write a component field declared as a deterministic type, with `CY_RESULT_PERMISSION_DENIED`.

#### Scenario: A fixed value round-trips
- **WHEN** a module writes a fixed-point component field through the ABI and reads it back
- **THEN** the raw integer SHALL be identical, with no conversion through floating point

#### Scenario: A float write to a fixed field is refused
- **WHEN** a module calls a floating-point setter on a deterministic field during a `Lockstep`
  session
- **THEN** the call SHALL return `CY_RESULT_PERMISSION_DENIED` with a message naming the field
