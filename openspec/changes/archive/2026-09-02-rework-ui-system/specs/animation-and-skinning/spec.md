## MODIFIED Requirements

### Requirement: Property animation and tweening
Beyond skeletal animation, the engine SHALL provide:

- **Property tracks** in clips, animating any reflected field
- **Tweens** — procedural, code-driven interpolation of properties with easing, sequencing,
  parallel groups, delays, callbacks, and looping

Tweens SHALL be bound to an entity so they are cancelled when it is destroyed.

Tweens SHALL be for **gameplay and scene properties**. UI animation is internal to the UI system
(see `ui-system`), so that UI transitions do not cross the scripting boundary per element per
frame and are not coupled to entity lifetime.

#### Scenario: Gameplay transition
- **WHEN** a tween animates a door entity's rotation with an ease-out curve over 0.3 s
- **THEN** it SHALL update per frame and complete with a callback

#### Scenario: Tween outlives its target
- **WHEN** a tween's target entity is destroyed
- **THEN** the tween SHALL be cancelled rather than writing to a dead entity

#### Scenario: UI transition
- **WHEN** a UI element transitions on hover, or a menu animates in
- **THEN** it SHALL be driven by the UI system's own animation rather than by an entity-bound
  tween, so it neither crosses the scripting boundary per frame nor depends on an entity's
  lifetime
