## ADDED Requirements

### Requirement: Persistent workspace control feedback
The application toolbar SHALL identify the current transform mode using the active semantic colour
and a persistent non-colour cue. This state SHALL follow the viewport's authoritative mode after
commands, shortcuts, and toolbar interaction. Resting application controls SHALL remain visually quiet
while hover and keyboard focus remain distinguishable.

#### Scenario: A shortcut changes the tool
- **WHEN** the user switches from Move to Rotate through the registered shortcut
- **THEN** the toolbar SHALL indicate Rotate as active and remove Move's active indication
- **AND** the indication SHALL remain distinguishable in monochrome

### Requirement: Selected dock tabs have a stable visual anchor
The selected tab in each dock region SHALL carry a thin selection-coloured underline, distinct from
ordinary tab text and from keyboard focus. Tab feedback SHALL NOT change the region's geometry.

#### Scenario: A diagnostics tab is selected
- **WHEN** the user switches from Console to Problems
- **THEN** the selection underline SHALL move to Problems without changing the panel's position

### Requirement: Panel summaries describe authoring content
Hierarchy and content-browser default summaries SHALL describe available content and relevant
selection rather than internal rebuild or preview-request counters. Diagnostic counters SHALL remain
accessible through diagnostic surfaces or tooltips.

#### Scenario: The hierarchy refreshes
- **WHEN** the hierarchy rebuilds without changing visible entities or selection
- **THEN** its default summary SHALL remain unchanged

#### Scenario: Search has no results
- **WHEN** an entity or asset search has no matches
- **THEN** the panel SHALL state that no matches were found and offer a relevant next step
- **AND** it SHALL distinguish this state from an empty world or empty asset list
