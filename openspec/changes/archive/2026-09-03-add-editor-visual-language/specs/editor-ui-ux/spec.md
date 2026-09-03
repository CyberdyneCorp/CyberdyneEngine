## MODIFIED Requirements

### Requirement: Familiarity is a feature
The editor's default interaction model SHALL be **deliberately familiar** to users of existing
engines: docked panels, a hierarchy, an inspector, a project browser, a viewport with standard
navigation, a play bar, and a console.

Departures from established conventions SHALL be **deliberate and justified by a measurable
improvement**, not incidental.

The editor SHALL ship a **Unity-compatible keymap** as a selectable preset, and SHALL support
user-defined keymaps.

An expert user's first hour SHALL not be spent relearning where things are.

**Familiarity is of structure and interaction, not of identity.** The editor SHALL be familiar in
where things are and how they behave — panels, hierarchy, inspector, viewport navigation, transform
tools, the play bar — and SHALL be distinctly itself in how it looks and what it calls things. Its
visual language, iconography, branding and vocabulary are specified in `editor-visual-language` and
SHALL NOT be borrowed from the engines whose layout conventions it deliberately adopts.

An expert arriving from another engine should find the controls where they expect them and know
immediately that this is not that engine.

#### Scenario: Familiar layout, distinct identity
- **WHEN** an experienced user opens the editor for the first time
- **THEN** the arrangement and interaction SHALL match their expectations, and the iconography,
  colour language, terminology and branding SHALL be recognisably Cyberdyne's own

#### Scenario: A Unity user is productive quickly
- **WHEN** a user selects the Unity keymap
- **THEN** navigation, transform tools, and common commands SHALL match their expectations

#### Scenario: A departure is recorded
- **WHEN** the editor differs from established convention
- **THEN** the reason SHALL be documented rather than assumed
