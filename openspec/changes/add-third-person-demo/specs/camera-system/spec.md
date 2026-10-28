## MODIFIED Requirements

### Requirement: Collision and occlusion are separate
The camera SHALL perform **collision** queries — keeping the camera out of geometry — and
**occlusion** queries — keeping the target visible — as distinct operations with distinct responses.

Responses SHALL be declared: pull in, slide, swap shoulder, fade the obstacle, or ignore by tag.

Queries SHALL be **batched** through the physics interface, and SHALL NOT be issued as scattered
synchronous casts from individual rig nodes.

Target visibility MAY sample several points on the target so that a partly occluded target is
distinguishable from a fully hidden one.

#### Scenario: Glass occludes without colliding
- **WHEN** a transparent surface stands between camera and target
- **THEN** the occlusion response SHALL apply, and the camera SHALL NOT be pushed as though it had
  collided

#### Scenario: Queries are batched
- **WHEN** several cameras evaluate collision and occlusion in a frame
- **THEN** their queries SHALL be batched rather than issued individually per node

#### Scenario: A camera pulled in by a wall stays in front of it
- **WHEN** a wall stands between the target and where the rig wants the camera, and the camera has
  been pulled in front of it
- **THEN** the next queries SHALL ask about the position the rig wants, not the pulled-in one, and
  the camera SHALL stay in front of the wall on every frame the wall is there

#### Scenario: A camera eases back out when the wall clears
- **WHEN** the wall that pulled the camera in is no longer in the way
- **THEN** the camera SHALL return to the distance the rig wants over the response's recovery
  half-life rather than in one frame, and a first contact SHALL put it in front of the wall at once

#### Scenario: A physics world answers the batch
- **WHEN** a rig's collision node has published a frame's queries and a physics world is present
- **THEN** a resolver SHALL answer each collision query with a sweep of the probe's radius and each
  occlusion query with a ray, passing through the bodies it is told to ignore, for the next
  evaluation
