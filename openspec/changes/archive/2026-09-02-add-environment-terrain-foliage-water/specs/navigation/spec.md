## ADDED Requirements

### Requirement: Environment-driven navigation
Navigation SHALL consume the environment as a first-class input:

- **Terrain** SHALL contribute a navigation surface derived from its collision representation,
  with slope limits applied (see `terrain`)
- **Terrain deformation** of the gameplay or structural class SHALL emit navigation dirty regions,
  rebuilt incrementally through the existing runtime update mechanism
- **Water** SHALL contribute swimming volumes, a vessel surface, and **directional cost** so that
  upstream travel costs more than downstream (see `water`)
- **Environment fields** MAY modify traversal cost through declared mappings — mud slowing
  movement, snow depth raising cost — rather than through bespoke code per effect

Cost mappings SHALL be data, declared once and applied uniformly, so that a new field influencing
movement does not require navigation changes.

#### Scenario: Digging changes routes
- **WHEN** a trench is excavated across a path
- **THEN** the affected navigation tiles SHALL be rebuilt and agents SHALL repath

#### Scenario: Rivers have a direction
- **WHEN** an agent paths along flowing water
- **THEN** downstream travel SHALL cost less than upstream

#### Scenario: A field changes cost without code
- **WHEN** a project declares that snow depth raises movement cost
- **THEN** the mapping SHALL be data and navigation SHALL apply it without modification
