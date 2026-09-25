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

#### Scenario: Scripted scene node
- **WHEN** a node names a registered Swift behaviour in `ScriptBehaviour.class` and the project module is built before Play
- **THEN** the runtime invokes its fixed update after physics while playing, suspends it during Pause, and restores the authored transform on Stop

#### Scenario: Edit a running Swift behaviour
- **WHEN** an author saves and builds a changed Swift source during Play
- **THEN** the editor requests a reload, the hosted runtime applies the new generation while preserving compatible live behaviour state, and the Swift Workspace reports success or a specific refusal

#### Scenario: Browse project Swift sources
- **WHEN** SwiftPM resolves dependencies under `.build` and an author opens the Swift Workspace
- **THEN** the source list shows project scripts without generated dependency checkouts, and the editor fills the available panel area while diagnostics remain visible
