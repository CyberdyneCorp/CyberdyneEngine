## REMOVED Requirements

### Requirement: Editor UI shares the runtime UI
**Reason**: Superseded by `editor-rust-application`. The editor is a separate Rust application built
with a Rust interface toolkit, so it cannot be CyberUI's most demanding consumer. Removing this
requirement rather than quietly leaving it unsatisfied is deliberate: the argument it made was a good
one, and the cost of losing it is recorded in the replacement requirement below.

**Migration**: The forcing function is replaced, not abandoned — see `Forcing functions for the UI
system`. The engine ships in-game tooling built on CyberUI, a conformance suite covering the same
demanding cases, and a real sample interface. That is weaker than dogfooding an editor, and the
specification says so rather than pretending otherwise.

## MODIFIED Requirements

### Requirement: Engine-owned UI system
The UI system SHALL be engine code: element storage, layout, styling, input routing, animation,
rendering, and authoring surfaces.

One system SHALL serve **game UI**, **world-space UI**, and the engine's **in-game tooling** —
developer console, debuggers, profiler overlays, and settings interfaces — rather than separate
technologies layered on each other.

The editor is not a consumer: it is a separate Rust application (see `editor-rust-application`), and
CyberUI SHALL NOT carry requirements that exist only to serve it.

The system SHALL be removable at build time via `CY_UI`.

#### Scenario: One system, three consumers
- **WHEN** a developer console, a game HUD, and a world-space health bar are all rendered
- **THEN** they SHALL use the same element storage, layout, styling, and rendering code

#### Scenario: No UI toolkit dependency
- **WHEN** the dependency manifest is audited
- **THEN** it SHALL contain no third-party UI toolkit; text remains delegated to `TextServer`

## ADDED Requirements

### Requirement: Forcing functions for the UI system
Because the editor is not built on CyberUI, the system SHALL have **explicit forcing functions** that
exercise it at the scale and complexity a production interface demands. Left implicit, a UI system
with no demanding first-party consumer decays.

The engine SHALL ship, built on CyberUI:

| Tooling | Exercises |
|---|---|
| Developer console | Text input, text editing, scrollback virtualisation, autocompletion, high message rates |
| Gameplay and network debuggers | Virtualised tables of tens of thousands of rows, live-updating values, filtering, sorting |
| Profiler and statistics overlays | Continuously changing content at frame rate with bounded cost |
| Settings and input rebinding interface | Property grids, navigation, controller and keyboard operation, modal flows |
| World-space and diegetic interface samples | World-space rendering, perspective interaction, and scaling |

These SHALL be **shipping features usable in a game**, not demonstrations, and SHALL be maintained as
such.

The engine SHALL additionally ship a **conformance suite** covering the demanding cases the editor
would otherwise have exercised — virtualised lists and trees of tens of thousands of rows, docking
layouts, reflection-generated property grids, graph canvases, and text editing — with performance
assertions that fail in continuous integration on regression.

A **sample project with a complete, real interface** SHALL be maintained, exercising navigation,
localisation, controller support, and animation end to end.

This is **weaker than dogfooding an editor**, and the requirement states it plainly so that the
weakness is managed rather than forgotten: a defect that only daily editor use would have found may
reach a game instead.

#### Scenario: Defects surface before games find them
- **WHEN** a virtualisation or text-editing defect is introduced
- **THEN** the conformance suite or the shipped in-game tooling SHALL surface it before a game does

#### Scenario: In-game tooling is real
- **WHEN** a game ships with the developer console enabled
- **THEN** it SHALL be a supported feature rather than a demonstration

#### Scenario: The weakness is acknowledged
- **WHEN** the forcing functions are evaluated
- **THEN** they SHALL be treated as a mitigation for the loss of editor dogfooding, and gaps SHALL
  be recorded rather than assumed covered

### Requirement: The runtime UI system is not the editor's toolkit
CyberUI SHALL be designed for **game and application interfaces**, and SHALL NOT carry requirements
that exist only to satisfy a desktop editor shell.

Where a capability is needed by both — virtualisation, docking, property grids, graph canvases, text
editing — it SHALL be justified by game and in-game tooling needs on its own merit.

The editor SHALL NOT be given a private CyberUI interface, since it does not use CyberUI at all;
correspondingly, no CyberUI feature SHALL be added solely because the editor once required it.

#### Scenario: A feature is justified on its own merit
- **WHEN** a CyberUI feature is proposed
- **THEN** its justification SHALL be a game or in-game tooling need, not the editor
