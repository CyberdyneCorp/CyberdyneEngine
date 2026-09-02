## MODIFIED Requirements

### Requirement: Geometry budget and importance
The system SHALL adjust the screen-error threshold to hold the **geometry allocation** issued by
the renderer budget arbiter (see `rendering-architecture`), raising the threshold under pressure
and lowering it when headroom exists within that allocation.

The system SHALL measure its own GPU cost and report it to the arbiter. It SHALL NOT measure total
frame time.

The system SHALL declare a **reserved minimum** quality and SHALL report when it has reached it,
so the arbiter reallocates rather than continuing to coarsen geometry that cannot coarsen further.

Adjustment SHALL be smooth and hysteretic: detail SHALL NOT visibly pump between frames, and SHALL
operate on a shorter time constant than the arbiter's reallocation.

Objects SHALL declare an **importance** — critical, gameplay, normal, background — scaling their
effective threshold, so gameplay-relevant geometry keeps detail while background geometry degrades
first.

The threshold SHALL be settable per view, and each view SHALL draw from its own allocation, so
secondary views cost less and degrade before the primary view does.

**Pinned mode** SHALL be global: when the arbiter is pinned, this controller SHALL be pinned with
it.

#### Scenario: Budget is held
- **WHEN** measured virtual geometry GPU time exceeds its allocation
- **THEN** the threshold SHALL be raised until the allocation is met, with the adjustment reported

#### Scenario: Important geometry keeps detail
- **WHEN** the scene is overloaded
- **THEN** background geometry SHALL coarsen before gameplay-critical geometry does

#### Scenario: No visible pumping
- **WHEN** load hovers around the allocation
- **THEN** hysteresis and rate limiting SHALL prevent detail from oscillating visibly

#### Scenario: Minimum reached
- **WHEN** geometry has coarsened to its declared minimum and the frame is still over budget
- **THEN** it SHALL report that it is at its minimum, and SHALL NOT coarsen further
