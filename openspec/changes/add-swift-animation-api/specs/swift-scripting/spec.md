# swift-scripting Spec Delta

## ADDED Requirements

### Requirement: Animation from Swift
CyberdyneKit SHALL expose the animation entries as an `Animator` value per entity — attach, play,
stop, set, fire, read the state, root motion and a joint's pose — and the frame's events as a list of
typed values, with names comparable against string literals.

#### Scenario: Units animate from the game
- **WHEN** a Swift game attaches an animator to every unit it enlists, plays walk while a unit's
  agent follows a path, a one-shot on arrival and idle again on the one-shot's own event
- **THEN** the engine SHALL animate the ordered unit through walk, the one-shot and idle while the
  other units stay idle, and with no game behaviour no unit SHALL be animated
