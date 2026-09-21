# Spec Delta

## ADDED Requirements

### Requirement: Runtime-authoritative editor play state
The editor SHALL present Playing or Paused only after the corresponding request is sent to an
attached engine runtime. Absence or loss of a runtime SHALL NOT end the authoring session and SHALL
leave or return every viewport to Editing with an actionable diagnostic.

#### Scenario: Play without a runtime
- **WHEN** a user requests Play while no engine runtime is attached
- **THEN** the editor SHALL remain in Editing
- **AND** it SHALL explain that no runtime is available to simulate the world

#### Scenario: Runtime is lost during play
- **WHEN** the attached runtime is lost while the editor presents Playing or Paused
- **THEN** the next editor pump SHALL return every viewport to Editing
- **AND** the editor SHALL preserve the open documents and their unsaved changes
