## MODIFIED Requirements

### Requirement: Deadline propagation
When a subsystem predicts a future need — the world predicting arrival at a region, a **sequence
declaring an upcoming shot or camera cut**, a teleport being initiated — that prediction SHALL be
expressed **once** as a deadline and propagated to every consumer: geometry pages, texture pages,
shadow warm-up, illumination prefetch, audio preload, and world cell preparation.

Subsystems SHALL NOT each derive the same prediction independently, since independent derivations
disagree about how soon.

A **compiled sequence** is the strongest predictor available to the engine: unlike camera
extrapolation, which estimates the next few hundred milliseconds, a sequence knows exactly where its
camera will be, which entities will be bound, and which assets each shot requires. Its preload plan
SHALL be expressible as deadlines through this mechanism.

A deadline SHALL be a scheduling input, consistent with the task system's rule that deadlines
influence when work runs and never what the simulation computes.

#### Scenario: One prediction, many preparations
- **WHEN** the world predicts the camera reaching a region in a known time
- **THEN** geometry, texture, shadow, illumination, and audio work for that region SHALL be
  scheduled against that deadline, from one prediction

#### Scenario: A camera cut is announced
- **WHEN** a cinematic declares a cut ahead of time
- **THEN** the destination's content SHALL be requested with an urgent deadline rather than
  discovered after the cut

#### Scenario: A shot list is a schedule
- **WHEN** a sequence begins
- **THEN** its preload plan SHALL become deadlines, so later shots' content is requested before they
  are reached
