## ADDED Requirements

### Requirement: Fog of war
The framework SHALL provide fog of war as SIMULATION STATE: a grid of cells over the world in
which, for each team, a cell is unexplored, explored, or visible, updated in the fixed step from
each unit's authoritative position and reveal radius. A cell SHALL be visible to a team when the
centre of the cell lies within the reveal radius of a unit of that team or of a team sharing its
vision; explored when it was ever visible to them; and unexplored otherwise. The update SHALL use
integer arithmetic only, so that every peer of a lockstep session reaches the same grid from the
same positions, and a digest of the grid SHALL be the same on every platform. How a team sees
another team's unit SHALL be visible when its cell is visible to the team, and otherwise hidden —
except a unit declared to leave a ghost (a structure), which a team that saw it SHALL remember
where it was last seen until the team sees that place again, when the ghost SHALL be refreshed if
the unit is still there and forgotten if it is not. A team SHALL always see its own units and its
allies'. An update naming a team out of range or two units with one identity SHALL be refused and
SHALL change nothing. A renderer SHALL only read the grid. A game SHALL be able to give a unit its
team, reveal radius and ghost through the native ABI in a fixed step or at initialisation, but not
in a frame update, and SHALL be able to ask, in any phase, how a team knows a cell and how it sees
a unit.

#### Scenario: A unit sees a disc of cells
- **WHEN** a unit stands at a cell's centre with a reveal radius of two cells
- **THEN** exactly the thirteen cells whose centres lie within two cells of it SHALL be visible to
  its team, and none SHALL be visible to another team

#### Scenario: What was seen stays explored
- **WHEN** a unit walks away from cells it saw
- **THEN** those cells SHALL be explored, not visible, for its team

#### Scenario: Allies share vision
- **WHEN** two teams share vision
- **THEN** each SHALL see the cells and the units the other's units see, and a third team SHALL not

#### Scenario: A structure is remembered and a unit is not
- **WHEN** a team sees another team's structure and its unit walks away
- **THEN** the structure SHALL be a ghost where it was last seen, a mobile unit that left the
  team's sight SHALL be hidden, and when the team sees the structure's place again after it was
  destroyed the ghost SHALL be forgotten

#### Scenario: Peers agree
- **WHEN** two grids are given the same updates on any platform
- **THEN** their digests SHALL be equal and SHALL equal the committed digest, and a peer that placed
  one unit elsewhere SHALL disagree

#### Scenario: A frame may not give sight
- **WHEN** a game gives a unit a vision in a frame update
- **THEN** the call SHALL be refused with a permission error, and the same call in a fixed step
  SHALL be accepted
