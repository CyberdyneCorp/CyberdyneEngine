# src/gameplay/fog/ — fog of war, the simulation half

`cy::gameplay-fog`. `FogOfWar`: what each team sees, has seen, and remembers, as lockstep state.

* **A grid of cells**, each holding one bit a team for "visible now" and one for "ever visible".
  Up to 32 teams (`TeamId` 1 to 32); `share_vision(a, b)` makes allies see through each other.
* **Integer arithmetic only.** A position is a Q32.32 metre (`detmath::Fixed::raw`, passed as the
  raw `i64`), located to 1/256 of a cell by integer division — flooring left of the origin — and a
  cell is in a unit's sight when its centre's squared distance, in those sub-cells, is no more than
  the squared reveal radius. `digest()` is the same on every platform; `unit.gameplay_fog` holds a
  committed one.
* **Units by sight.** `sight(team, id)`: VISIBLE when the unit's cell is visible to the team (its
  own and its allies' always are), HIDDEN otherwise — except a unit that `leaves_ghost` (a
  structure): a team that saw it remembers it where it was last seen (`ghost(team, id)`) until it
  sees that place again, when the ghost is refreshed or, if the structure is gone, forgotten.
* **A refused update changes nothing**: a team past 32 or two units with one id.

A renderer reads `write_states(team, bytes)` and nothing else: `src/rendering/fog_of_war/` draws it,
and `cy::game-backend-fog` (src/game_backend/fog/) is ABI 1.10's adapter over it. Vision is a
radius on a flat grid: there is no line-of-sight occlusion.
