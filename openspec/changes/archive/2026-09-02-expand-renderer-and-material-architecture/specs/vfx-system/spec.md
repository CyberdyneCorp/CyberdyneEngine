## MODIFIED Requirements

### Requirement: Importance classes and frame-budget scalability
Every effect SHALL declare an **importance class**: `Critical`, `Important`, `Ambient`, or
`Decorative`.

A **VFX budget controller** SHALL hold the VFX allocation issued by the renderer budget arbiter
(see `rendering-architecture`), adjusting: spawn rates, simulation frequency, particle count caps,
collision quality, renderer feature level (lighting, shadows, sorting), and effect LOD.

The controller SHALL measure VFX cost and report it to the arbiter. It SHALL NOT measure total
frame time or infer global load, because a cost VFX did not incur is not a cost VFX should
correct for.

The controller SHALL declare a **reserved minimum**, and SHALL report when it has reached it so
the arbiter can reallocate rather than continue reducing a subsystem with nothing left to give.

Adjustment SHALL proceed from least to most important: `Decorative` first, `Critical` last.
`Critical` effects SHALL have reserved capacity and SHALL be degraded only when no other headroom
remains.

Adjustments SHALL be applied smoothly and hysteretically so quality does not visibly oscillate,
and SHALL operate on a shorter time constant than the arbiter's reallocation.

**Pinned mode** SHALL be global: when the arbiter is pinned, this controller SHALL be pinned with
it.

#### Scenario: Battle exceeds the budget
- **WHEN** measured VFX GPU time is 3.4 ms against a 2.0 ms allocation
- **THEN** the controller SHALL reduce decorative and ambient cost first, and SHALL report which
  levers it applied and the resulting time

#### Scenario: Gameplay-legible effects survive
- **WHEN** the scene is heavily overloaded
- **THEN** `Critical` effects SHALL still render at their configured minimum quality

#### Scenario: Pinned mode for capture
- **WHEN** pinned mode is enabled
- **THEN** no adaptive adjustment SHALL occur, and exceeding the budget SHALL be reported rather
  than corrected

#### Scenario: No visible oscillation
- **WHEN** load hovers around the allocation
- **THEN** hysteresis SHALL prevent quality from flickering between levels

#### Scenario: VFX does not pay for another subsystem's cost
- **WHEN** the frame is slow because of geometry cost while VFX is within its allocation
- **THEN** the VFX controller SHALL make no adjustment
