## MODIFIED Requirements

### Requirement: Multiple worlds
The engine SHALL support multiple concurrently existing `World` instances — for example the
edited world and the play-mode world, or a client-prediction world and a server-authoritative
one — each with independent entities, archetypes, and schedules.

`World` SHALL refer exclusively to this runtime container. The spatial and persistence layer
defined in `world-partition-and-streaming` SHALL NOT be called a world in the API; it publishes
entities into a `World` and is addressed by its own names.

A `World` SHALL support **bulk entity creation** from prepared archetype blocks — allocating chunks
and copying component columns without per-entity construction — since that is how cooked cells and
entity templates are instantiated.

#### Scenario: Editor and play-mode coexist
- **WHEN** the editor enters play mode
- **THEN** the played world SHALL be a separate `World` instance so editing state is untouched

#### Scenario: The name is not overloaded
- **WHEN** a developer encounters `World` in the API
- **THEN** it SHALL always mean the ECS runtime container

#### Scenario: Bulk instantiation
- **WHEN** a cooked cell containing prepared archetype blocks is activated
- **THEN** the world SHALL allocate chunks and copy component columns in bulk, rather than
  creating entities one at a time
