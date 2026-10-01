# navigation Spec Delta

## ADDED Requirements

### Requirement: Deterministic navigation arithmetic
Each navigation world SHALL declare its arithmetic: floating point, or the deterministic math types
of `deterministic-math`.

A world declaring deterministic arithmetic SHALL use those types for every authoritative result:
path costs and heuristics, funnel orientation tests, flow-field integration costs, and crowd
avoidance. Equal candidates SHALL still be broken by stable identity. It SHALL convert its baked
navigation mesh to deterministic types once, at load, before the first tick.

Path following, local avoidance, and crowd steering SHALL be written once and instantiated for both
kinds of arithmetic, rather than maintained as two diverging implementations.

Runtime navigation-mesh rebuilding SHALL NOT be an authoritative input to a world declaring
deterministic arithmetic. Dynamic obstacles in such a world SHALL be expressed through integer
polygon flags and flow-field cost overlays.

The navigation subsystem SHALL declare the `CrossPlatform` determinism profile only when every
authoritative navigation world declares deterministic arithmetic.

#### Scenario: The same path on two architectures
- **WHEN** the same path query runs in a deterministic-arithmetic world on x86-64 and on arm64
- **THEN** the corridor, the point path, and the reported cost SHALL be bit-identical

#### Scenario: A runtime rebuild under lockstep is refused
- **WHEN** a deterministic-arithmetic world in a `Lockstep` session requests a runtime tile rebuild
  as authoritative input
- **THEN** the request SHALL be refused with a diagnostic naming the world, and an obstacle SHALL be
  expressible as a cost overlay instead

#### Scenario: A float world names itself
- **WHEN** a `Lockstep` session includes an authoritative navigation world declaring floating-point
  arithmetic
- **THEN** configuration SHALL be refused naming the navigation subsystem
