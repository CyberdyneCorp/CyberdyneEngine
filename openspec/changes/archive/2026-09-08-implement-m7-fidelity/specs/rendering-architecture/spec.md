## MODIFIED Requirements

### Requirement: Renderer budget arbiter
The renderer SHALL contain exactly one **budget arbiter**, which measures frame cost and
distributes a frame budget as **allocations** to subsystems: geometry, shadows, global
illumination, reflections, material evaluation, VFX, post-processing, and resolution scale.

Subsystem controllers SHALL hold their own allocation using their own levers. They SHALL report
their measured cost to the arbiter, and SHALL NOT independently measure total frame time or infer
global load.

The arbiter SHALL adjust allocations on a **longer time constant** than the subsystem controllers
adjust within them, so a subsystem is never tracking a moving target.

Each subsystem SHALL declare a **reserved minimum**. A subsystem at its minimum SHALL report so,
and the arbiter SHALL reallocate from subsystems with headroom rather than continuing to reduce
one that has none.

A **pinned mode** SHALL disable the arbiter and every subsystem controller together, for
cinematics, capture, benchmarking, and deterministic tests. Partial pinning SHALL NOT be
possible.

The arbiter SHALL report, per frame: each allocation, each subsystem's measured cost, which
subsystems are at their minimum, and every adjustment made with its cause.

**The convergence guarantee has an operating envelope, and it is stated in the arbiter's own
units.** The arbiter SHALL settle without oscillation whenever the nominal state — every subsystem
at authored quality — leaves at least **four deadbands** of headroom below the frame budget, where
the deadband is the width the arbiter derives from the coarsest reachable lever quantum. Outside
that envelope the arbiter SHALL still reallocate, still respect every reserved minimum and still
restore authored quality when load lifts, but it MAY continue moving levers after the load returns
to nominal, and the guarantee does not hold.

The envelope is expressed in deadbands rather than milliseconds because the deadband is a property
of the content's lever ladder, not of a frame rate: the same absolute headroom is comfortable for
content with fine levers and marginal for content with coarse ones.

A configuration whose nominal state falls inside the envelope SHALL be reported at configuration
time, not discovered as jitter at runtime.

**What is measured, and what is not.** The arbiter drives the **filtered** frame time to
`budget - deadband`. An individual frame is that plus the scene's own noise, so a claim that no
single frame ever exceeds the budget is a claim the control law does not make and SHALL NOT be
asserted of it. Frames over budget SHALL be reported as a figure; the settled filtered frame is
what "the budget is held" means.

#### Scenario: Controllers do not fight
- **WHEN** heavy geometry pushes the frame over budget
- **THEN** the arbiter SHALL reduce the geometry allocation, and the VFX and post-processing
  controllers SHALL NOT independently reduce quality for a cost they did not incur

#### Scenario: Resolution and quality do not oscillate
- **WHEN** dynamic resolution reduces internal resolution, lowering every subsystem's measured
  cost
- **THEN** allocations SHALL be re-evaluated by the arbiter alone, on its own time constant,
  rather than every controller reacting to the apparent headroom

#### Scenario: Convergence is checked over the operating point, not only over the load
- **WHEN** the arbiter's convergence is verified
- **THEN** the verification SHALL sweep the **nominal cost** as well as the step magnitude, and
  SHALL assert the envelope — because one nominal cost can make any control law look settled, for
  the same reason one step magnitude can

#### Scenario: Inside the envelope
- **WHEN** the nominal state leaves fewer than four deadbands of headroom
- **THEN** the configuration SHALL be reported as outside the convergence guarantee, and a test
  asserting no oscillation there SHALL NOT be treated as a regression when it fails

#### Scenario: A subsystem with nothing left to give
- **WHEN** virtual geometry has reached its minimum quality and the frame is still over budget
- **THEN** it SHALL report that it is at its minimum and the arbiter SHALL take the shortfall from
  a subsystem with headroom

#### Scenario: Pinned mode is total
- **WHEN** pinned mode is enabled for a capture
- **THEN** the arbiter and every subsystem controller SHALL stop adapting, and budget overruns
  SHALL be reported rather than corrected

#### Scenario: Adjustments are attributable
- **WHEN** quality drops during play
- **THEN** the report SHALL state which allocation changed, by how much, and what measurement
  caused it
