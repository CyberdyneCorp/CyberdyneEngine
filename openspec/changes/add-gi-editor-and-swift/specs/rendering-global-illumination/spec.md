## ADDED Requirements

### Requirement: Run-time illumination control
A configured illumination system SHALL accept, without a rebuild, the settings a player's options
menu, a level script and an editor change: its mode, its global quality and tier preference (the
values GI volumes override), whether hardware ray tracing may be used, and its budget; a rule per
material, per instance or both over what the emissive classification makes of an emitter
(automatic, cache only, promoted, off), the most specific rule deciding; and an object's
illumination class, or a return to the class its mobility and size derive.

Each SHALL change only what it names: a system none of them was called on, or called with what it
already had, SHALL resolve every point to the same bits as the configured system. A change of global
quality or tier SHALL NOT discard what the radiance cache gathered, and an emitter rule SHALL NOT
discard what the surface cache gathered for cards whose emission it did not change.

The same control SHALL be reachable from a game through the native ABI, in presentation phases only,
and from the editor as undoable commands each with an agent tool of the same effect; and a game
SHALL be able to sample the indirect light arriving at a point, the resolve's own answer.

#### Scenario: A lamp is put out and lit again
- **WHEN** a game switches an emitter's material off and then back to automatic
- **THEN** its promoted light SHALL leave and return, the light it gave SHALL leave the surfaces it
  lit, no cell SHALL be ingested again, and once lit again the surfaces SHALL be shaded to the same
  bits as before it went out

#### Scenario: A player raises the GI quality
- **WHEN** the global quality is raised while the world runs
- **THEN** every probe SHALL gather at the new quality from its next update, and the indirect light
  SHALL NOT drop in the meantime

#### Scenario: A game reads how lit a body is
- **WHEN** a game samples the indirect light at a point with no surface normal
- **THEN** it SHALL receive the mean over the six axis directions of the resolve's own answer, and
  the same call SHALL be refused in a fixed simulation step

#### Scenario: An author sees every GI view in the editor's viewport
- **WHEN** an author selects a GI debug view in the editor
- **THEN** the editor-hosted runtime SHALL draw it over its own frame from the authored world's GI,
  and returning to the lit view SHALL give the frame it gave before
