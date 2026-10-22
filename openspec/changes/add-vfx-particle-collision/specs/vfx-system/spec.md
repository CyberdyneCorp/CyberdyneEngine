## MODIFIED Requirements

### Requirement: Collision
Particles SHALL support collision against: analytic primitives (plane, sphere, box, capsule), the
**scene signed distance field**, the **depth buffer** in screen space, and **physics queries** for
the CPU path.

Each mode SHALL declare its accuracy and cost, and its limitations SHALL be documented — notably
that depth-buffer collision cannot see off-screen or occluded geometry.

Collision response SHALL support: bounce with restitution, friction, sliding, kill, and raising a
GPU event.

Collision quality SHALL be adjustable by the budget controller.

A collider SHALL be a graph node whose outputs — the resolved position, the responded velocity,
whether the particle touches, whether a contact began this step, the impact speed of a contact that
began, and the surface normal — are pure values over one contact query, so an author composes the
response from them and a stage queries the world once however many outputs it reads. The scene
signed distance field SHALL be the global illumination's own distance field, read on the device
from its device copy rather than from a second one. On the CPU path an emitter SHALL be able to
spawn one particle per event another emitter raised, at that event's payload. An emitter that
collides with nothing SHALL generate and bind exactly what it did before collision existed.

#### Scenario: SDF collision against complex geometry
- **WHEN** particles collide using the scene SDF
- **THEN** they SHALL respond to arbitrary static geometry including geometry outside the view

#### Scenario: Depth-buffer limitation
- **WHEN** depth-buffer collision is used
- **THEN** particles SHALL pass through off-screen and occluded geometry, and this SHALL be
  documented rather than treated as a defect

#### Scenario: Collision raises an event
- **WHEN** a particle collides and its response is configured to raise an event
- **THEN** the event SHALL be written to the GPU event buffer for consumption by another emitter

#### Scenario: A falling particle comes to rest on a plane
- **WHEN** particles fall onto a plane with a restitution below one and a rest speed
- **THEN** they SHALL come to rest one radius above it, within a tenth of a millimetre, and still

#### Scenario: Restitution is the bounce
- **WHEN** a particle closes on a surface at speed v with restitution e
- **THEN** it SHALL leave along the surface normal at e times v, to float precision

#### Scenario: An event fires once per contact
- **WHEN** a particle bounces several times and then rests on a surface
- **THEN** its impact event SHALL be raised once for every bounce and never while it rests

#### Scenario: The device agrees with the CPU path
- **WHEN** one colliding effect is simulated on the device and by the CPU executor
- **THEN** the two SHALL agree slot for slot about which particles live, particle for particle about
  where they are within a bound stated for each collider, and in the number of impact events

#### Scenario: Collision off is the frame before
- **WHEN** an effect has no collider
- **THEN** its generated units and cook key SHALL be byte-identical to those of the tree before
  collision existed
