// SPDX-License-Identifier: MIT
#pragma once
// Fog of war: what each team has seen, what it sees now, and what it remembers.
// `gameplay-framework` — "Fog of war".
//
// ================================================================================================
// THE GRID IS SIMULATION STATE, AND IT IS INTEGERS
// ================================================================================================
//
// Which cells a team sees decides what its player may order, and in a lockstep session every peer
// must reach the same answer from the same command log. So the grid is updated in the fixed step
// from the units' authoritative positions, in INTEGER arithmetic only: a position is a Q32.32 metre
// (`detmath::Fixed::raw`, passed as the raw `i64` so that this module links no math library), it
// is located on the grid to 1/256 of a cell by integer division, and a cell is in a unit's sight
// when the squared distance from its centre, in those sub-cells, is no more than the squared
// radius. Nothing rounds, nothing contracts, and `digest()` is the same on every platform.
//
// A renderer READS this state — `write_states` — and never writes it: the soft edges, the fade
// and the colours are presentation (`src/rendering/fog_of_war/`), and a frame that drew the fog
// differently cannot make two peers disagree.
//
// ================================================================================================
// THREE STATES A CELL, AND TWO WAYS TO SEE A UNIT
// ================================================================================================
//
//   Unexplored  no unit of the team (or of a team sharing its vision) has ever seen it
//   Explored    seen before, not seen now
//   Visible     in the sight of at least one such unit this tick
//
// Another team's unit is `Visible` to a team when its cell is, `Hidden` when it is not — a moving
// unit is not remembered — and `Ghost` when it LEAVES A GHOST (a structure:
// `FogUnit::leaves_ghost`) and the team saw it once: the ghost stays at the place it was last seen
// until the team sees that place again, when it is refreshed if the unit is still there and
// forgotten if it is not.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/gameplay/teams.h>

namespace cy::gameplay::fog {

/// Teams one grid tracks: one bit each of a cell's two masks. `TeamId` 1 to 32; `kNoTeam` gives no
/// sight and is seen by everyone whose sight covers it.
inline constexpr u32 kMaxFogTeams = 32;
/// The largest grid edge, in cells.
inline constexpr u32 kMaxFogCells = 4096;
/// A position is located to 1/`kSubCells` of a cell.
inline constexpr i64 kSubCells = 256;
/// The largest reveal radius, in cells. A longer one is clamped to it.
inline constexpr i64 kMaxRevealCells = 1024;
/// The raw value of one metre: `detmath::Fixed::kOneRaw`.
inline constexpr i64 kOneMetreRaw = i64{1} << 32;

enum class CellState : u8 {
    Unexplored = 0,
    Explored = 1,
    Visible = 2,
};

[[nodiscard]] const char* cell_state_name(CellState state) noexcept;

/// How a team sees another team's unit.
enum class UnitSight : u8 {
    Hidden = 0,
    /// Not seen now; drawn where it was last seen. Only a unit that leaves a ghost.
    Ghost = 1,
    Visible = 2,
};

[[nodiscard]] const char* unit_sight_name(UnitSight sight) noexcept;

struct FogGridDescription {
    /// Cells along x and along z. 1 to `kMaxFogCells` each.
    u32 width = 0;
    u32 height = 0;
    /// The edge of a cell, Q32.32 metres. Positive, and at most 2^20 metres.
    i64 cell_size_raw = kOneMetreRaw;
    /// The world position of cell (0, 0)'s minimum corner, Q32.32 metres.
    i64 origin_x_raw = 0;
    i64 origin_z_raw = 0;
};

/// One unit, as the fixed step hands it to `update`.
struct FogUnit {
    /// Stable across ticks: a persistent identity or an entity's bits. Unique within one update.
    u64 id = 0;
    TeamId team = kNoTeam;
    /// Its authoritative position on the ground plane, Q32.32 metres.
    i64 x_raw = 0;
    i64 z_raw = 0;
    /// How far it sees, Q32.32 metres. Zero sees nothing, not even its own cell.
    i64 reveal_radius_raw = 0;
    /// A structure: another team that saw it remembers it as a ghost.
    bool leaves_ghost = false;
};

/// Where a team last saw a unit that leaves a ghost.
struct FogGhost {
    TeamId observer = kNoTeam;
    u64 id = 0;
    TeamId owner = kNoTeam;
    i64 x_raw = 0;
    i64 z_raw = 0;
    /// The tick it was last seen at.
    u64 seen_tick = 0;
};

/// What the last update did.
struct FogStatistics {
    u64 tick = 0;
    u32 units = 0;
    /// Cells some unit's sight reached, counted once each.
    u32 cells_visible_any = 0;
    u32 ghosts = 0;
};

class FogOfWar {
public:
    explicit FogOfWar(Allocator& allocator) noexcept;

    FogOfWar(const FogOfWar&) = delete;
    FogOfWar& operator=(const FogOfWar&) = delete;

    /// Size the grid. Every cell starts unexplored for every team, and every team sees alone.
    [[nodiscard]] Status create(const FogGridDescription& description) noexcept;
    [[nodiscard]] bool created() const noexcept { return width_ != 0; }
    [[nodiscard]] const FogGridDescription& description() const noexcept { return description_; }
    [[nodiscard]] u32 width() const noexcept { return width_; }
    [[nodiscard]] u32 height() const noexcept { return height_; }

    /// `a` and `b` see what either sees, from the next update: allies sharing vision. Symmetric.
    [[nodiscard]] Status share_vision(TeamId a, TeamId b) noexcept;

    /// One fixed step: every unit's sight stamped, explored cells grown, ghosts refreshed and
    /// forgotten. Refused, changing nothing, when a unit names a team past `kMaxFogTeams` or two
    /// units share an id.
    [[nodiscard]] Status update(Span<const FogUnit> units, u64 tick) noexcept;

    /// A cell's state for `team`. Out of range, or `kNoTeam`, is `Unexplored`.
    [[nodiscard]] CellState cell(TeamId team, u32 x, u32 z) const noexcept;
    /// The state of the cell holding a world position. Off the grid is `Unexplored`.
    [[nodiscard]] CellState at(TeamId team, i64 x_raw, i64 z_raw) const noexcept;
    /// How `observer` sees the unit `id` of the last update. Its own team's units and its allies'
    /// are `Visible`; a unit the last update did not hold is `Ghost` if `observer` remembers it and
    /// `Hidden` otherwise.
    [[nodiscard]] UnitSight sight(TeamId observer, u64 id) const noexcept;
    /// The ghost `observer` holds of `id`, or null.
    [[nodiscard]] const FogGhost* ghost(TeamId observer, u64 id) const noexcept;
    /// Every ghost, ordered by observer then id. A ghost whose unit the observer sees now is not
    /// drawn as one — ask `sight`.
    [[nodiscard]] Span<const FogGhost> ghosts() const noexcept { return ghosts_.span(); }

    /// `team`'s state of every cell, one `CellState` byte a cell, row-major from cell (0, 0) with
    /// x fastest: what a renderer uploads. `out` holds `width() * height()` bytes.
    [[nodiscard]] Status write_states(TeamId team, Span<u8> out) const noexcept;

    /// The cell holding a world position, or false when it is off the grid.
    [[nodiscard]] bool locate(i64 x_raw, i64 z_raw, u32& x, u32& z) const noexcept;

    /// A hash of every cell's masks, every ghost and the tick: equal on every peer of a lockstep
    /// session after the same updates.
    [[nodiscard]] u64 digest() const noexcept;
    [[nodiscard]] const FogStatistics& statistics() const noexcept { return statistics_; }

private:
    struct Seen {
        u64 id = 0;
        TeamId team = kNoTeam;
        /// The teams that see it this tick, one bit each.
        u32 seen_by = 0;
    };

    [[nodiscard]] Status check(Span<const FogUnit> units) noexcept;
    void stamp(const FogUnit& unit) noexcept;
    [[nodiscard]] u32 observers_of(u32 cell) const noexcept;
    [[nodiscard]] Status record_units(Span<const FogUnit> units, u64 tick) noexcept;
    [[nodiscard]] Status refresh_ghost(TeamId observer, const FogUnit& unit, u64 tick) noexcept;
    void forget_ghosts() noexcept;
    [[nodiscard]] usize ghost_index(TeamId observer, u64 id) const noexcept;
    /// The bits of the teams `team` sees through: its own and its allies'.
    [[nodiscard]] u32 vision_of(TeamId team) const noexcept;

    Allocator* allocator_;
    FogGridDescription description_{};
    u32 width_ = 0;
    u32 height_ = 0;
    /// One bit a team: whose sight covers the cell this tick, and whose ever has.
    Array<u32> visible_;
    Array<u32> explored_;
    /// `share_[t - 1]`: the teams whose sight team t sees through, itself included.
    u32 share_[kMaxFogTeams] = {};
    Array<Seen> seen_;
    HashMap<u64, u32> seen_index_;
    /// The next update's index, built beside `seen_index_` and swapped with it.
    HashMap<u64, u32> next_index_;
    Array<FogGhost> ghosts_;
    FogStatistics statistics_{};
};

}  // namespace cy::gameplay::fog
