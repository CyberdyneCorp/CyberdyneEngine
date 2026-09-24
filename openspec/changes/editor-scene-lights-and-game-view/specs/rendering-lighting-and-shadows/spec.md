# Spec Delta

## ADDED Requirements

### Requirement: Hosted authored lighting
The hosted editor runtime SHALL extract enabled light components from the authored world with composed node transforms and submit them to the same scene renderer used for the Game image. Light field or transform edits SHALL affect a subsequent frame without a restart. A world with no authored lights SHALL not gain an unrequested game light.

#### Scenario: Light moves
- **WHEN** a point light is moved in Editor view
- **THEN** its illumination moves in the next rendered frames and the saved position survives reopening

#### Scenario: Empty world
- **WHEN** a world has no enabled lights
- **THEN** Game view receives no authored light

#### Scenario: Toggle and aim an authored light
- **WHEN** an author disables the last light or rotates an enabled directional or spot light
- **THEN** the next Editor and Game frames reflect the new illumination; a disabled light remains selectable in Editor view

### Requirement: Authored directional shadows
An enabled directional light with `casts_shadow` SHALL cast shadows from authored mesh casters onto authored mesh receivers in the hosted Editor and Game images. Moving or rotating the light or meshes SHALL update the shadow. Disabling the light or its shadow flag SHALL remove the shadow. A point light's rotation SHALL not change its illumination because it emits in every direction.

#### Scenario: Tree over a Plane
- **WHEN** an imported tree stands over a Plane and a shadow-casting directional light shines on both
- **THEN** the Plane shows the tree's silhouette, and rotating or disabling the light changes or removes it in the next rendered frames
