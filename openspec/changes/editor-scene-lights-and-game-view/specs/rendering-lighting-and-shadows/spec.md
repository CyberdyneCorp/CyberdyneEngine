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
