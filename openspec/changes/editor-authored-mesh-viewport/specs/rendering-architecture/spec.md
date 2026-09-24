# Spec Delta

## ADDED Requirements

### Requirement: Authored world frame submission
The editor's engine host SHALL submit visible authored mesh instances to the same frame assembly and recording path used by an engine view on every enabled native graphics backend. Its output SHALL include their geometry and transforms, and a backend's render capture SHALL assert image content rather than only command submission.

#### Scenario: Metal frame capture
- **WHEN** the standard frame records a lit mesh on Metal
- **THEN** the captured output SHALL contain shaded mesh pixels and differ from the frame recorded without draw callbacks.

#### Scenario: Viewport uses the authored world
- **WHEN** a world transaction adds a mesh instance or changes its transform
- **THEN** the next completed viewport frame SHALL use the updated world state.
