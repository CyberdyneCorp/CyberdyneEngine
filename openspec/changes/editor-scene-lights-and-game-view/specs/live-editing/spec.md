# Spec Delta

## ADDED Requirements

### Requirement: Play view and runtime execution status
Entering Play SHALL switch the visible scene to Game view, run the fixed-step physics session, and execute available project gameplay and audio services. Pausing SHALL suspend simulation progression and Stop SHALL restore the authored world and Editor view. The editor SHALL report unavailable script or audio services explicitly rather than imply they ran.

#### Scenario: Play then Stop
- **WHEN** an author presses Play and then Stop
- **THEN** physics advances only during Play, the Game image is shown during Play, and the authored Editor view is restored afterward

#### Scenario: Missing runtime service
- **WHEN** a project requests gameplay script or audio execution but the hosted runtime lacks that service
- **THEN** the editor reports which service is unavailable
