## ADDED Requirements

### Requirement: The visible frame executes temporal anti-aliasing
The renderer SHALL execute temporal anti-aliasing when the post chain enables it and SHALL feed its resolved color into subsequent post-processing.

#### Scenario: A temporal frame is recorded
- **WHEN** temporal anti-aliasing is enabled on a device-backed assembled frame
- **THEN** the temporal pass SHALL read current color, motion, depth, and previous history
- **AND** it SHALL write the current history before tone mapping reads it

### Requirement: Temporal history persists and invalidates safely
The renderer SHALL retain temporal history across successful frames and SHALL reject it after an invalidation event.

#### Scenario: The first frame or a cut is rendered
- **WHEN** no valid previous history exists or the temporal framework reports invalidation
- **THEN** the temporal resolve SHALL use the current frame without blending stale history

#### Scenario: Consecutive stable frames are rendered
- **WHEN** a temporal frame executes successfully after a valid temporal frame
- **THEN** the renderer SHALL reproject and blend the previous completed history
- **AND** it SHALL advance the history ping-pong index only after successful execution
