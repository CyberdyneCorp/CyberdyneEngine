## MODIFIED Requirements

### Requirement: Live prefab update
In development builds, editing a prefab while the game is running SHALL update existing instances by
diffing the previous compiled template against the new one.

This SHALL be an instance of the general contract in `live-editing`: the live edit compiler produces
a runtime delta, the applicable **live edit policy** determines how it is applied, and **field
classification** determines what is preserved — `Authoring` fields updated, `RuntimeState` and
`PersistentState` preserved, `Derived` recomputed.

Added components SHALL be added, removed components removed, and instance overrides reapplied.

Instances whose state cannot be reconciled SHALL be reported rather than silently reset, and the
policy SHALL be configurable per prefab.

Shipping builds SHALL retain no prefab link, and this behaviour SHALL be absent from them.

#### Scenario: Live edit preserves gameplay state
- **WHEN** a designer changes a robot prefab's maximum health while the game runs
- **THEN** existing robots SHALL take the new maximum and keep their current health

#### Scenario: Shipping builds carry no provenance
- **WHEN** a shipping build spawns a prefab
- **THEN** the resulting entities SHALL carry no prefab link and no override data

#### Scenario: A structural change announces its policy
- **WHEN** a prefab edit requires recreating instances rather than updating them
- **THEN** the applicable policy SHALL be reported before the change is applied
