## ADDED Requirements

### Requirement: A subsystem controller's reported cost is measured, not tabulated
"Subsystem controllers SHALL report their measured cost to the arbiter" is satisfied today by no
instance under `src/`: the controllers that exist live in a sample over a hard-coded cost table, and
two engine modules declare a priced lever ladder to the arbiter without ever reporting what a frame
actually cost them. **A declaration is not a measurement**, and an arbiter fed constants converges
beautifully on a model of the frame rather than on the frame.

A subsystem controller SHALL report a cost derived from an observation of its own work in a frame —
a timer, a counter scaled by a measured unit cost, or a device query — and SHALL NOT report a value
that is a compile-time constant or a table lookup independent of the frame.

The arbiter's per-frame report SHALL distinguish a measured cost from an estimated one, so that a
subsystem with no measurement is visible as such rather than indistinguishable from one that is
cheap.

#### Scenario: A constant is not a measurement
- **WHEN** a subsystem controller reports a cost that does not vary with the work it did that frame
- **THEN** the arbiter's report SHALL mark that subsystem's cost as estimated, and the check SHALL
  fail if a subsystem claiming to report a measured cost never varies

#### Scenario: An engine subsystem reports to the arbiter
- **WHEN** the engine runs a frame with a subsystem under arbiter control
- **THEN** at least one controller outside a sample SHALL report its measured cost, and the
  allocation it receives SHALL respond to that cost changing
