## ADDED Requirements

### Requirement: Window coordinates mapped into the view
The platform's input source SHALL convert every pointer position and relative motion it forwards
from the window's coordinates into those of the view the game reads, by a scale and an offset the
host sets, so that a view drawn scaled and letterboxed into a window of another size receives the
pointer in its own pixels. The mapping SHALL be the identity until a host sets one, and SHALL NOT
apply to the wheel.

#### Scenario: A letterboxed view
- **WHEN** a view of 1280 by 720 is drawn into a 2560 by 1600 window with 80-pixel bars and the pointer moves to the window's (1280, 880) by (8, -6)
- **THEN** the input server SHALL receive the position (640, 400) and the motion (4, 3)

### Requirement: A headless host's platform events
A host with no display server SHALL be able to bring the platform's event subsystem up alone, push
synthetic wheel events as well as keys, pointer motion and buttons into the platform's queue, and
empty that queue itself, so that a game a person would play in a window is driven through the
platform's input path on a machine with no display.

#### Scenario: A wheel with no window
- **WHEN** a wheel's turn of two notches is pushed and the queue drained, with only the event subsystem up
- **THEN** the input server SHALL receive the wheel's two notches as a physical event, and the platform's queue SHALL be empty
