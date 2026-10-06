# animation-and-skinning Spec Delta

## ADDED Requirements

### Requirement: Device pose buffer from the dirty range
The renderer SHALL own one device pose buffer for the GPU pose world and SHALL fill it from the pose
world's dirty range — the matrices published since the last upload — at the world's own indices,
never by copying the whole world, so that an instance's `matrix_offset` is the offset a skinning
dispatch reads.

#### Scenario: A changed instance uploads only its range
- **WHEN** one of three instances publishes a new pose and another's unpublished half holds data the
  world never committed
- **THEN** the upload SHALL write exactly the published instance's matrices, and the uncommitted
  data SHALL NOT reach the device

#### Scenario: Nothing published
- **WHEN** a frame publishes no pose
- **THEN** the upload SHALL write nothing
