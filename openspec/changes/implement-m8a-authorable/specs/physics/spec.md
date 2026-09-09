## ADDED Requirements

### Requirement: Physics world teardown
Destroying a physics world SHALL be safe while the step that used it still has work in flight, and
SHALL release every body, shape and interpolation record the world owned.

The order SHALL be defined and enforced by the code that owns it rather than left to a caller:
bodies before shapes, the shapes before the world, and the world before the server. A teardown that
overtook a worker still holding a job the backend was about to free is the defect this requirement
exists to prevent, and it presents as a fault with no physics call on the stack.

Tearing down and rebuilding a world SHALL be an ordinary operation rather than a once-per-process
one. Play mode creates and destroys a world on every press of play, and a leak of one shape or one
body per session is a leak nobody notices until the fortieth run.

#### Scenario: A world torn down mid-step under load
- **WHEN** a world with bodies is destroyed immediately after a step, repeatedly, with the machine's
  cores saturated
- **THEN** no fault SHALL occur and the job system SHALL still be running afterwards

#### Scenario: The world dies before the things holding its bodies
- **WHEN** a physics world is destroyed while a bridge still holds handles to bodies in it
- **THEN** releasing those handles SHALL report a diagnostic and SHALL NOT fault

#### Scenario: A session leaves nothing behind
- **WHEN** a world with colliders is torn down
- **THEN** every shape the run created SHALL be released, so a request for the same shape afterwards
  allocates a new one rather than returning the cached handle
