## MODIFIED Requirements

### Requirement: XR is deferred but not precluded
XR SHALL NOT be implemented in the initial engine milestone. The architecture SHALL, however,
satisfy the structural prerequisites listed in this specification, and any change that would
violate them SHALL be treated as a breaking architectural decision.

**At 1.0 the deferral SHALL be restated rather than lapsing**, with its re-entry point — after 1.0 —
and with **every prerequisite check passing on the day 1.0 is recorded**. A deferral that survives to
a release gate without its seams re-verified is a deferral that was forgotten rather than kept.

**A prerequisite that is only partly open SHALL be recorded as partly open**, naming which half does
not hold and what closing the gap would cost. It SHALL NOT be reported as satisfied because the
check that exists passes: a check covering one half of a prerequisite is evidence about that half
and silence about the other, and silence recorded as a pass is the failure mode this whole
specification exists to prevent.

Before 1.0 is recorded, each partly-open prerequisite SHALL be resolved deliberately — either the
open half is completed, or its closure is accepted as an explicit decision through an OpenSpec change
against this specification, stating what XR would then cost to add.

#### Scenario: A change would preclude XR
- **WHEN** a proposal assumes a single view per camera, or a single fixed frame rate, or that the
  application owns the main loop
- **THEN** it SHALL be flagged against this specification and either revised or accepted with an
  explicit decision to drop XR support

#### Scenario: The deferral is restated at 1.0
- **WHEN** the 1.0 record is written
- **THEN** `xr-support` SHALL appear in it as deferred with its re-entry point, and every
  prerequisite check SHALL have been run and passed as part of that gate

#### Scenario: A prerequisite is half open at the gate
- **WHEN** a prerequisite's check passes while a documented half of that prerequisite is known not to
  hold
- **THEN** the record SHALL name the open half and its cost, and 1.0 SHALL NOT be recorded until that
  half is either completed or closed by an explicit decision

### Requirement: Runtime-driven frame timing prerequisite
The runtime SHALL support an execution model in which **an external runtime drives frames**: the
engine exposes a `tick()` entry point and does not assume ownership of a `while (running)` loop.

The frame loop SHALL tolerate a frame cadence and predicted display time supplied externally,
rather than deriving all timing from its own clock.

**This prerequisite has two halves and only one of them is open.** The half that is open is the
renderer's: the frame's inputs arrive as arguments rather than being read from a clock, and the check
that proves it compares command streams across two renders separated by other work. The half that is
**not** open is the runtime's: `tick()` accepts no predicted display time, so a host cannot tell the
engine when the frame it is about to produce will be displayed — which is the entire content of
"predicted display time" and the reason an XR runtime supplies one.

**The gap SHALL be named in the record rather than absorbed by the passing half's check.** Before 1.0
either `tick()` SHALL accept an externally supplied predicted display time and the check SHALL cover
it, or the partial closure SHALL be recorded as a decision with its cost — stated as what an XR port
would have to change, since that is the number the deferral exists to keep small.

#### Scenario: Runtime calls the engine
- **WHEN** an XR runtime signals that a frame should begin, with a predicted display time
- **THEN** the engine SHALL run one frame against that time, rather than free-running

#### Scenario: The predicted display time reaches the frame
- **WHEN** a host supplies a predicted display time to the runtime's frame entry point
- **THEN** the frame SHALL be produced against that time, and the check SHALL fail if the time is
  read from the runtime's own clock instead

#### Scenario: The open half is recorded, not assumed
- **WHEN** the renderer's half of this prerequisite passes and the runtime's half does not hold
- **THEN** the record SHALL state which half is open and what closing it would cost, and the passing
  check SHALL NOT be read as satisfying the prerequisite
