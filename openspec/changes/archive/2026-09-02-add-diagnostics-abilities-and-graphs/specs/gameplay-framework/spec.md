## MODIFIED Requirements

### Requirement: Scope and extension
This capability SHALL cover universal game structure only. Abilities, attributes, inventories,
quests, and objectives SHALL be **optional modules built on it**, not parts of it, so that a game
does not pay for systems it does not use.

Game-specific concepts — resources, weapons, crafting, mana — SHALL remain game code.

Optional gameplay modules SHALL be separately activatable features. **Abilities, effects, and
attributes are provided by `gameplay-abilities-and-effects`** on exactly these terms: a project that
does not enable it links none of its code and carries none of its data. Inventories, quests, and
objectives remain unspecified and SHALL follow the same pattern when added.

**Gameplay visual scripting is provided by `visual-scripting`.** The seam this requirement reserved
is honoured: commands, events, tags, rules, and validation are reflected schemas, and gameplay graphs
**compile to systems** rather than being interpreted per entity. That constraint SHALL be preserved
against future proposals.

#### Scenario: A game without abilities pays nothing
- **WHEN** a project does not use the ability module
- **THEN** no ability code or data SHALL be present in its build

#### Scenario: The scripting seam is preserved
- **WHEN** a proposal would make commands or validation unable to be expressed as compiled data
- **THEN** it SHALL be flagged against this requirement

#### Scenario: Graphs compile, they do not interpret
- **WHEN** gameplay logic is authored as a graph
- **THEN** it SHALL compile to a system over archetypes, with no interpreter instance per entity
