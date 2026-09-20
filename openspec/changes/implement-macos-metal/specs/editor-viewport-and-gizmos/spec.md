## ADDED Requirements

### Requirement: macOS hosted frame ownership
The macOS hosted viewport transport SHALL present engine-rendered frames with their originating frame
identity, generation, and view state. A shared surface SHALL NOT be reused by the producer until the
consumer's use has completed. Resize and runtime restart SHALL invalidate obsolete surface generations.

#### Scenario: A consumer is behind the producer
- **WHEN** the editor still samples a surface while a newer engine frame becomes ready
- **THEN** the producer SHALL preserve the sampled surface until the consumer releases it
- **AND** picking SHALL refer to the frame actually displayed

#### Scenario: The runtime restarts after resizing
- **WHEN** a previous surface generation remains cached in the editor
- **THEN** it SHALL NOT be accepted as a frame from the restarted runtime
- **AND** the viewport SHALL report unavailable or stale state until a valid new frame arrives
