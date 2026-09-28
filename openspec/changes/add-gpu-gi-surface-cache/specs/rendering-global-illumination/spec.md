## ADDED Requirements

### Requirement: The GI scene and the surface cache run on a device behind the host seams
The engine SHALL be able to hold the GI scene's distance-field clipmaps and surface cards on a
graphics device and shade the surface cache's cards in compute, and SHALL do so behind the seams the
host subsystems already meet through, so that choosing the device or the host is one installation
and no other GI subsystem changes.

The device copy SHALL be updated incrementally from what changed — the bricks the field re-solved
and the cards whose state the host changed — and SHALL NOT be rebuilt every frame. The device card
update SHALL shade exactly the budgeted, prioritised selection the host update would shade, and
SHALL compute each card's direct light (shadowed through a shadow map for the directional light it
was captured for and through the distance field otherwise), the sky term, and the previous frame's
card radiance as the next bounce. The host implementation SHALL remain, and SHALL be the oracle
every device result is compared against.

#### Scenario: The device trace agrees with the host trace
- **WHEN** a batch of rays is sphere traced against the uploaded field on the device and against the
  host field
- **THEN** the hit or miss, the distance and the normal SHALL agree within a stated bound

#### Scenario: A moved object invalidates only its bricks
- **WHEN** a placed object moves and the field scrolls
- **THEN** the device copy SHALL receive exactly the bricks the host re-solved, all of them near where
  the object was or is
- **AND** a frame in which nothing moved SHALL upload nothing

#### Scenario: Device card radiance matches the host surface cache
- **WHEN** the same cards are shaded on the device and on the host over the same scene for several
  frames
- **THEN** each card's direct and accumulated radiance SHALL agree within a stated bound

#### Scenario: The device update respects its budget
- **WHEN** a budgeted update runs on the device
- **THEN** it SHALL shade exactly the pages the host selection chose, no more than the budget, and
  SHALL leave every other page unchanged
