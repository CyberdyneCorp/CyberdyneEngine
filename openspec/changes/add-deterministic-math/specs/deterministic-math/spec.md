# deterministic-math Spec Delta

## ADDED Requirements

### Requirement: Deterministic math module
The engine SHALL provide an optional module of **deterministic math types** whose results are
bit-identical on every supported compiler, optimisation level, vector width, and architecture,
because every operation is defined in integer arithmetic.

The module SHALL live at layer 0, SHALL depend on no module above `core`, and SHALL NOT be linked by
sessions that do not use it.

No public operation of the module SHALL read a floating-point value, call the C mathematics library,
or depend on the floating-point environment, except the named conversion functions.

#### Scenario: The kernel contains no float
- **WHEN** the module's kernel translation units are compiled with general-purpose registers only,
  on a compiler that accepts that restriction
- **THEN** they SHALL compile, and introducing a floating-point operation into a kernel SHALL fail
  that build

#### Scenario: Optional by construction
- **WHEN** a session declares `SamePlatform`, `ReplayStable`, or `None`
- **THEN** it SHALL NOT be required to link the module

### Requirement: Fixed-point formats
The module SHALL provide these formats, each for a declared use:

| Type | Representation | Use |
|---|---|---|
| `Fixed` | Q32.32 in a signed 64-bit integer | All authoritative arithmetic |
| `Fixed16` | Q16.16 in a signed 32-bit integer | Compact storage only; widened to `Fixed` before arithmetic |
| `Angle` | Unsigned 32-bit binary angle, one turn = 2^32 | Headings and trigonometric arguments and results |
| `WideFixed` | Q64.64 | Products, dot products, and squared lengths, so that comparisons cannot overflow |

Units SHALL remain metres, seconds, kilograms, and radians, except that `Angle` measures turns.

`Fixed` positions SHALL be world-absolute.

#### Scenario: A squared distance cannot overflow
- **WHEN** two units 100 km apart compare their squared distance with a radius
- **THEN** the comparison SHALL be computed in `WideFixed` and SHALL be exact

#### Scenario: Storage is not arithmetic
- **WHEN** a component stores a position as `Fixed16`
- **THEN** reading it SHALL widen it to `Fixed`, and no arithmetic operator SHALL be defined on
  `Fixed16`

### Requirement: Fixed-point arithmetic rules
The rules of arithmetic SHALL be specified as the contract every implementation meets bit for bit.
Implementations include compiler intrinsics, a portable reference, and other languages.

- Addition, subtraction, and negation SHALL wrap in two's complement, and SHALL NOT be undefined
  behaviour.
- Multiplication SHALL form the exact 128-bit product, round to nearest with ties toward positive
  infinity, and wrap to 64 bits.
- Division SHALL truncate the exact quotient toward zero. Division by zero SHALL return the maximum
  value for a non-negative dividend and the minimum value for a negative one, in every build.
- Comparison SHALL be total: there is no NaN, infinity, or negative zero.

Overflow SHALL wrap in every build. Development builds SHALL count overflows and name the first
site. Shipping builds SHALL carry no cost for that counting.

A portable reference implementation SHALL be compiled on every platform, and any intrinsic path
SHALL be tested against it bit for bit.

#### Scenario: Intrinsic and reference agree
- **WHEN** the multiply and divide intrinsics selected on a platform are run over the golden
  vectors and a random sweep
- **THEN** every result SHALL equal the portable reference's result

#### Scenario: Overflow is deterministic and reported
- **WHEN** an authoritative computation overflows in a development build
- **THEN** every peer SHALL compute the same wrapped value, and the overflow guard SHALL report the
  first site

### Requirement: Deterministic transcendental functions
The module SHALL provide `sqrt`, `sin`, `cos`, `sincos`, `tan`, `atan`, `atan2`, `asin`, `acos`,
`exp2`, `log2`, `exp`, and `log`.

Each SHALL be computed by exact range reduction and a fixed-point polynomial, using integer
operations only.

Each function SHALL have a **declared error bound**, measured against the exact function of its
quantised input. `sqrt` SHALL be correctly rounded. `sin` and `cos` SHALL be within one unit in the
last place of `Fixed`. `atan2` SHALL be within one unit in the last place of `Angle`.

Polynomial coefficients SHALL be integer constants produced by a committed generator. The generator
SHALL record each polynomial's measured error beside its coefficients.

Out-of-domain inputs SHALL return a defined value and SHALL be counted in development builds:

- a negative argument to `sqrt` or `log2`;
- an argument outside [-1, 1] to `asin` or `acos`;
- an `exp2` result beyond range.

#### Scenario: The bound is checked, not assumed
- **WHEN** the property tests evaluate a function at the oracle's inputs
- **THEN** every error SHALL be within the declared bound, and the worst error found SHALL be
  reported beside the bound

#### Scenario: A loosened bound is a reviewed change
- **WHEN** a function cannot meet its declared bound
- **THEN** the bound SHALL change in the same change with a recorded reason, and SHALL NOT be relaxed
  silently

### Requirement: Deterministic vectors, rotations, and transforms
The module SHALL provide `FixedVec2`, `FixedVec3`, `Fixed16Vec3`, `Rot2`, `FixedQuat`,
`FixedTransform`, `FixedAabb`, `FixedCircle`, and `FixedCapsule2D`. These types SHALL follow the
coordinate conventions of `core-math`.

Dot products and squared lengths SHALL return `WideFixed`. Lengths, distances, and normalisation
SHALL use the square root of the wide value.

Quaternion composition SHALL renormalise its result.

The module SHALL NOT provide projective matrices or interpolation for presentation. Those are
computed in floating point after conversion.

#### Scenario: Conventions agree with core math
- **WHEN** an identity `FixedTransform` is queried for its forward, up, and right vectors
- **THEN** they SHALL be `(0, 0, -1)`, `(0, 1, 0)`, and `(1, 0, 0)`, as for `core-math`'s
  `Transform`

#### Scenario: Normalising zero
- **WHEN** a zero vector is normalised
- **THEN** the result SHALL be the zero vector, and development builds SHALL count the call

### Requirement: The conversion boundary
Floating-point values SHALL be converted to deterministic types only at three moments:

- at **cook**, where the cooked asset stores the raw integer;
- at **session configuration**, before the first tick, where the results enter the state hash;
- at **command creation** on the issuing peer, where the command carries the converted value.

No conversion from floating point SHALL occur inside a tick on an authoritative path.

Conversion to floating point SHALL be for presentation only. It SHALL be performed relative to a
`Fixed` origin, so that camera-relative rendering is exact up to the final rounding. Its result
SHALL be presentation-classified.

The conversion entry points SHALL be few and greppable, so their call sites can be audited.

#### Scenario: A pick becomes a command, not a float in the tick
- **WHEN** a player clicks a destination
- **THEN** the issuing peer SHALL convert the picked position to `Fixed` and record it in the command
  payload, and every other peer SHALL read those bits rather than converting anything

#### Scenario: Rendering a distant unit
- **WHEN** a unit 500 km from the world origin is rendered
- **THEN** its position SHALL be converted relative to the camera, and the presentation value SHALL
  NOT be readable by an authoritative system

### Requirement: Deterministic math versioning
The module SHALL declare a **kernel version** that changes whenever any function's output changes
for any input. Examples are a coefficient, a reduction, and a rounding rule.

The kernel version SHALL be recorded wherever a reproducibility claim depends on it: replay
manifests, lockstep compatibility scopes, and published cross-architecture digests.

Golden outputs SHALL be regenerated only by an explicit recording mode that fails the run in which
it records.

#### Scenario: A changed coefficient is a version change
- **WHEN** a polynomial coefficient changes
- **THEN** the kernel version SHALL change in the same change, and a replay recorded with the old
  version SHALL be classified incompatible with that reason

### Requirement: Cross-architecture proof of deterministic math
The module's determinism SHALL be proven by evidence that can fail. The evidence SHALL include:

- **golden vectors** of inputs and committed raw outputs for every public function, checked on
  every continuous-integration leg on its own;
- a **kernel digest** and a **fixed-point movement digest**, published by every cross-architecture
  leg and compared by the existing cross-leg comparator. The comparator SHALL apply its refusals for
  a single leg, a single architecture, a zero digest, and an empty workload;
- **property tests** against a high-precision oracle, asserting each declared error bound and the
  monotonicity and identity properties of the functions;
- a comparison, in one process, of the kernel built at the baseline instruction set and at a wider
  vector width, with identical results;
- recorded falsification of each check by a mutation that turns it red.

The cross-platform claim SHALL extend exactly as far as the architectures and operating systems
whose legs published and agreed, and the documentation SHALL name them.

#### Scenario: One leg diverges
- **WHEN** a single leg computes a different `sin` output for one golden input
- **THEN** that leg's own test SHALL fail, without waiting for the cross-leg comparison

#### Scenario: Two architectures agree
- **WHEN** the x86-64 and arm64 legs publish their kernel and movement digests
- **THEN** the comparator SHALL report agreement only if every compared digest is identical and
  non-empty on both

#### Scenario: Vector width does not change results
- **WHEN** the kernel is compiled at the baseline instruction set and with a 256-bit vector
  extension and run over the same inputs
- **THEN** the digests SHALL be identical

### Requirement: Deterministic math performance
The module SHALL publish a performance budget and defend it with registered benchmarks with
baselines:

- per-operation costs for multiply, divide, `sqrt`, `sin`/`cos`, `atan2`, `exp2`, and `log2`;
- a movement step of 100 000 units with separation.

The ratio of the fixed-point movement step to the same kernel in `f32` SHALL be reported, so that
the cost of the `CrossPlatform` profile is visible rather than assumed.

#### Scenario: The cost of cross-platform determinism is a number
- **WHEN** the benchmarks run
- **THEN** they SHALL report the fixed-point movement step's time and its ratio to the `f32` kernel,
  and a regression past the baseline SHALL fail the run
