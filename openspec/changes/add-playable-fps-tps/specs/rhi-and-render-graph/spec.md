## ADDED Requirements

### Requirement: A resized swapchain keeps its handle
Resizing a swapchain SHALL rebuild it under the handle the caller holds, and a rebuild that fails
SHALL leave the previous swapchain as it was.

#### Scenario: A window resized twice keeps presenting
- **WHEN** a played game's window is resized twice while it presents
- **THEN** every frame SHALL still be presented, with no validation error
