# Spec Delta

## ADDED Requirements

### Requirement: Editor and Game views
The editor SHALL expose an Editor view using its own camera and authoring gizmos and a Game view using an enabled scene camera. The Game image SHALL exclude selection marks, transform and light gizmos, orientation controls, and diagnostic overlays. Changing views SHALL preserve the Editor camera.

#### Scenario: Compare an authored scene
- **WHEN** an author selects Game view without entering Play
- **THEN** the scene appears through the scene camera without editor-only marks and the simulation does not start

#### Scenario: No scene camera
- **WHEN** no enabled scene camera exists
- **THEN** Game view displays a clear missing-camera message rather than an unrelated editor camera image

### Requirement: Light authoring handles
The Editor view SHALL show selectable light handles at authored light positions, including the light type. These handles SHALL not appear in Game view.

#### Scenario: Placing a point light
- **WHEN** an author creates and selects a point light
- **THEN** its handle and transform gizmo appear in Editor view and its illumination appears in the rendered scene

### Requirement: Camera and light direction handles
The Editor view SHALL show a selectable handle at each authored Camera and light, and a direction indicator for Cameras, directional lights, and spot lights based on their composed world transform. A selected actor SHALL expose its transform and component properties in the Inspector. Game view SHALL omit these handles and indicators.

#### Scenario: Aim a Camera or directional light
- **WHEN** an author rotates a Camera or directional light in the scene
- **THEN** its Editor direction indicator updates to the transformed forward axis and selecting its handle exposes its editable properties
