## ADDED Requirements

### Requirement: First-person weapon view
A first-person camera's weapon view SHALL draw the layers it selects — the arms and the weapon —
with its own field of view and near plane into the primary view's frame, so that it shares that
frame's depth, lighting and temporal history. Its depth SHALL be placed in front of every world
surface beyond a stated distance of the world's near plane, so that a weapon pushed into a wall is
drawn whole. The primary view SHALL NOT draw the weapon view's layers.

The weapon view SHALL be lit, fogged and shadowed at its world position, SHALL receive the world's
shadows and cast none by default with each settable, and SHALL write motion vectors from its own
previous projection, so that a weapon carried by a turning camera has none.

#### Scenario: A weapon against a wall
- **WHEN** a weapon on the weapon view's layers reaches into a wall nearer than its muzzle
- **THEN** every texel the weapon covers in open air SHALL be covered by the weapon

#### Scenario: The world's lens changes
- **WHEN** the primary view's field of view changes and the weapon view's does not
- **THEN** the weapon's texels and their shading SHALL not change

#### Scenario: Off is the frame before
- **WHEN** a frame has no weapon view, or one that selects no instance
- **THEN** it SHALL be the frame drawn before the weapon view existed

### Requirement: First-person camera behaviours
The engine SHALL provide first-person camera behaviours as a rig parameter producer: mouse look
with a sensitivity applied once, head bob by distance walked, recoil as an impulse into a damped
spring that recovers to the aim, and aiming down the sights blending the world's lens and the weapon
view's. Each SHALL be frame-rate independent, and motion reduction SHALL scale the bob and the view's
share of the recoil together. View aim, control aim and weapon aim SHALL be derived separately: the
recoil the view does not take turns the weapon.

#### Scenario: Recoil at two frame rates
- **WHEN** the same kick is integrated at 60 and at 144 hertz
- **THEN** the aim SHALL follow the same curve and recover over the same wall-clock time
