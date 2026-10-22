// SPDX-License-Identifier: MIT
#include <cy/gameplay/fog/fog_of_war.h>

#include <algorithm>
#include <functional>
#include <utility>

namespace cy::gameplay::fog {
namespace {

/// The smallest cell: a position is located to 1/256 of a cell, and a cell below 1/256 m would put
/// that below the Q32.32 resolution a sub-cell product is computed at.
constexpr i64 kMinCellRaw = i64{1} << 24;
/// The largest cell, 2^20 m: a remainder times `kSubCells` then stays inside 63 bits.
constexpr i64 kMaxCellRaw = i64{1} << 52;

[[nodiscard]] constexpr u32 bit(TeamId team) noexcept {
    return u32{1} << (team - 1U);
}

[[nodiscard]] constexpr bool valid_team(TeamId team) noexcept {
    return team != kNoTeam && team <= kMaxFogTeams;
}

/// `a - b`, wrapping as Q32.32 arithmetic does rather than overflowing.
[[nodiscard]] constexpr i64 wrapping_sub(i64 a, i64 b) noexcept {
    return static_cast<i64>(static_cast<u64>(a) - static_cast<u64>(b));
}

/// The quotient rounded toward negative infinity. `divisor` is positive.
[[nodiscard]] constexpr i64 floor_div(i64 value, i64 divisor) noexcept {
    const i64 quotient = value / divisor;
    return (value % divisor != 0 && value < 0) ? quotient - 1 : quotient;
}

/// A length in Q32.32 metres as 1/256 cells, rounded down, without a 128-bit product: the whole
/// cells times 256, plus the remainder's share of one.
[[nodiscard]] constexpr i64 to_sub_cells(i64 value, i64 cell) noexcept {
    const i64 whole = floor_div(value, cell);
    const i64 remainder = value - (whole * cell);
    return (whole * kSubCells) + ((remainder * kSubCells) / cell);
}

/// One 64-bit word folded into the digest: FNV-1a's offset and prime applied a word at a time,
/// with the high half folded down after each multiply so every bit of the word reaches every bit
/// of the hash. Integer arithmetic on values, never on bytes in memory, so it is the same on every
/// platform and either byte order.
constexpr u64 kFnvOffset = 0xCBF29CE484222325ULL;
constexpr u64 kFnvPrime = 0x100000001B3ULL;

[[nodiscard]] constexpr u64 mix(u64 hash, u64 word) noexcept {
    hash = (hash ^ word) * kFnvPrime;
    return hash ^ (hash >> 32U);
}

}  // namespace

const char* cell_state_name(CellState state) noexcept {
    switch (state) {
        case CellState::Unexplored:
            return "unexplored";
        case CellState::Explored:
            return "explored";
        case CellState::Visible:
            return "visible";
    }
    return "unknown";
}

const char* unit_sight_name(UnitSight sight) noexcept {
    switch (sight) {
        case UnitSight::Hidden:
            return "hidden";
        case UnitSight::Ghost:
            return "ghost";
        case UnitSight::Visible:
            return "visible";
    }
    return "unknown";
}

FogOfWar::FogOfWar(Allocator& allocator) noexcept
    : visible_(allocator),
      explored_(allocator),
      seen_(allocator),
      seen_index_(allocator),
      next_index_(allocator),
      ghosts_(allocator) {}

Status FogOfWar::create(const FogGridDescription& description) noexcept {
    if (description.width == 0 || description.height == 0 || description.width > kMaxFogCells ||
        description.height > kMaxFogCells) {
        return fail(ErrorCode::InvalidArgument, "fog of war: a grid edge is 1 to 4096 cells");
    }
    if (description.cell_size_raw < kMinCellRaw || description.cell_size_raw > kMaxCellRaw) {
        return fail(ErrorCode::InvalidArgument, "fog of war: a cell is 1/256 m to 2^20 m across");
    }
    const usize cells = static_cast<usize>(description.width) * description.height;
    if (Status sized = visible_.resize(cells); !sized) {
        return sized;
    }
    if (Status sized = explored_.resize(cells); !sized) {
        return sized;
    }
    std::ranges::fill(visible_, 0U);
    std::ranges::fill(explored_, 0U);
    for (u32 team = 1; team <= kMaxFogTeams; ++team) {
        share_[team - 1U] = bit(team);
    }
    seen_.clear();
    seen_index_.clear();
    ghosts_.clear();
    statistics_ = FogStatistics{};
    description_ = description;
    width_ = description.width;
    height_ = description.height;
    return ok();
}

Status FogOfWar::share_vision(TeamId a, TeamId b) noexcept {
    if (!valid_team(a) || !valid_team(b)) {
        return fail(ErrorCode::InvalidArgument, "fog of war: a team is 1 to 32");
    }
    share_[a - 1U] |= bit(b);
    share_[b - 1U] |= bit(a);
    return ok();
}

u32 FogOfWar::vision_of(TeamId team) const noexcept {
    return valid_team(team) ? share_[team - 1U] : 0U;
}

bool FogOfWar::locate(i64 x_raw, i64 z_raw, u32& x, u32& z) const noexcept {
    if (!created()) {
        return false;
    }
    const i64 cell = description_.cell_size_raw;
    const i64 column = floor_div(wrapping_sub(x_raw, description_.origin_x_raw), cell);
    const i64 row = floor_div(wrapping_sub(z_raw, description_.origin_z_raw), cell);
    if (column < 0 || row < 0 || std::cmp_greater_equal(column, width_) ||
        std::cmp_greater_equal(row, height_)) {
        return false;
    }
    x = static_cast<u32>(column);
    z = static_cast<u32>(row);
    return true;
}

Status FogOfWar::check(Span<const FogUnit> units) noexcept {
    // The next tick's index is built beside the current one and swapped in only when every unit
    // passed, so a refused update leaves the current one, and no tick allocates once both have
    // grown to the unit count.
    HashMap<u64, u32>& ids = next_index_;
    ids.clear();
    if (Status reserved = ids.reserve(units.size()); !reserved) {
        return reserved;
    }
    for (usize index = 0; index < units.size(); ++index) {
        const FogUnit& unit = units[index];
        if (unit.team > kMaxFogTeams) {
            return fail(ErrorCode::InvalidArgument, "fog of war: a unit's team is past 32");
        }
        if (ids.contains(unit.id)) {
            return fail(ErrorCode::InvalidArgument, "fog of war: two units share an id");
        }
        if (Expected<u32*, Error> added = ids.insert(unit.id, static_cast<u32>(index));
            !added.has_value()) {
            return make_unexpected(added.error());
        }
    }
    std::swap(seen_index_, next_index_);
    return ok();
}

void FogOfWar::stamp(const FogUnit& unit) noexcept {
    if (!valid_team(unit.team) || unit.reveal_radius_raw <= 0) {
        return;
    }
    const i64 cell = description_.cell_size_raw;
    const i64 cx = to_sub_cells(wrapping_sub(unit.x_raw, description_.origin_x_raw), cell);
    const i64 cz = to_sub_cells(wrapping_sub(unit.z_raw, description_.origin_z_raw), cell);
    const i64 radius =
        std::min(to_sub_cells(unit.reveal_radius_raw, cell), kMaxRevealCells * kSubCells);
    // Every cell whose centre may be inside the disc, clamped to the grid. A unit far off the grid
    // leaves the range empty before anything is squared.
    const i64 first_x = std::max<i64>(floor_div(cx - radius, kSubCells), 0);
    const i64 last_x = std::min<i64>(floor_div(cx + radius, kSubCells), width_ - 1);
    const i64 first_z = std::max<i64>(floor_div(cz - radius, kSubCells), 0);
    const i64 last_z = std::min<i64>(floor_div(cz + radius, kSubCells), height_ - 1);
    const i64 reach = radius * radius;
    const u32 mask = bit(unit.team);
    for (i64 z = first_z; z <= last_z; ++z) {
        const i64 dz = (z * kSubCells) + (kSubCells / 2) - cz;
        const i64 dz2 = dz * dz;
        if (dz2 > reach) {
            continue;
        }
        u32* row = visible_.data() + (static_cast<usize>(z) * width_);
        for (i64 x = first_x; x <= last_x; ++x) {
            const i64 dx = (x * kSubCells) + (kSubCells / 2) - cx;
            if ((dx * dx) + dz2 <= reach) {
                row[x] |= mask;
            }
        }
    }
}

u32 FogOfWar::observers_of(u32 cell) const noexcept {
    const u32 covered = visible_[cell];
    if (covered == 0U) {
        return 0U;
    }
    u32 observers = 0;
    for (u32 team = 1; team <= kMaxFogTeams; ++team) {
        if ((covered & share_[team - 1U]) != 0U) {
            observers |= bit(team);
        }
    }
    return observers;
}

usize FogOfWar::ghost_index(TeamId observer, u64 id) const noexcept {
    const auto* const found = std::ranges::lower_bound(
        ghosts_, std::pair<TeamId, u64>{observer, id}, std::less<>{},
        [](const FogGhost& ghost) { return std::pair<TeamId, u64>{ghost.observer, ghost.id}; });
    if (found != ghosts_.end() && found->observer == observer && found->id == id) {
        return static_cast<usize>(found - ghosts_.begin());
    }
    return ghosts_.size();
}

Status FogOfWar::refresh_ghost(TeamId observer, const FogUnit& unit, u64 tick) noexcept {
    FogGhost ghost;
    ghost.observer = observer;
    ghost.id = unit.id;
    ghost.owner = unit.team;
    ghost.x_raw = unit.x_raw;
    ghost.z_raw = unit.z_raw;
    ghost.seen_tick = tick;
    const usize index = ghost_index(observer, unit.id);
    if (index < ghosts_.size()) {
        ghosts_[index] = ghost;
        return ok();
    }
    if (Status added = ghosts_.push_back(ghost); !added) {
        return added;
    }
    // Kept ordered by observer then id, so lookups bisect and the digest walks one order.
    std::rotate(std::upper_bound(ghosts_.begin(), ghosts_.end() - 1, ghost,
                                 [](const FogGhost& a, const FogGhost& b) {
                                     return a.observer != b.observer ? a.observer < b.observer
                                                                     : a.id < b.id;
                                 }),
                ghosts_.end() - 1, ghosts_.end());
    return ok();
}

Status FogOfWar::record_units(Span<const FogUnit> units, u64 tick) noexcept {
    if (Status sized = seen_.resize(units.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < units.size(); ++index) {
        const FogUnit& unit = units[index];
        u32 x = 0;
        u32 z = 0;
        const u32 observers =
            locate(unit.x_raw, unit.z_raw, x, z) ? observers_of((z * width_) + x) : 0U;
        seen_[index] = Seen{unit.id, unit.team, observers};
        if (!unit.leaves_ghost) {
            continue;
        }
        // A team remembers another's structure; its own and its allies' it simply sees.
        const u32 owners = unit.team == kNoTeam ? 0U : vision_of(unit.team);
        for (u32 team = 1; team <= kMaxFogTeams; ++team) {
            if ((observers & bit(team)) == 0U || (owners & bit(team)) != 0U) {
                continue;
            }
            if (Status refreshed = refresh_ghost(team, unit, tick); !refreshed) {
                return refreshed;
            }
        }
    }
    return ok();
}

void FogOfWar::forget_ghosts() noexcept {
    // A ghost whose place its observer sees now and whose unit it does not see there is gone: the
    // structure was destroyed, or is somewhere else.
    usize kept = 0;
    for (const FogGhost& ghost : ghosts_) {
        bool keep = at(ghost.observer, ghost.x_raw, ghost.z_raw) != CellState::Visible;
        if (!keep) {
            const u32* slot = seen_index_.find(ghost.id);
            keep = slot != nullptr && (seen_[*slot].seen_by & bit(ghost.observer)) != 0U;
        }
        if (keep) {
            ghosts_[kept++] = ghost;
        }
    }
    (void)ghosts_.resize(kept);
}

Status FogOfWar::update(Span<const FogUnit> units, u64 tick) noexcept {
    if (!created()) {
        return fail(ErrorCode::InvalidArgument, "fog of war: update before create");
    }
    if (Status checked = check(units); !checked) {
        return checked;
    }
    std::ranges::fill(visible_, 0U);
    for (const FogUnit& unit : units) {
        stamp(unit);
    }
    u32 covered = 0;
    for (usize cell = 0; cell < visible_.size(); ++cell) {
        explored_[cell] |= visible_[cell];
        covered += visible_[cell] != 0U ? 1U : 0U;
    }
    if (Status recorded = record_units(units, tick); !recorded) {
        return recorded;
    }
    forget_ghosts();
    statistics_.tick = tick;
    statistics_.units = static_cast<u32>(units.size());
    statistics_.cells_visible_any = covered;
    statistics_.ghosts = static_cast<u32>(ghosts_.size());
    return ok();
}

CellState FogOfWar::cell(TeamId team, u32 x, u32 z) const noexcept {
    const u32 vision = vision_of(team);
    if (vision == 0U || x >= width_ || z >= height_) {
        return CellState::Unexplored;
    }
    const usize index = (static_cast<usize>(z) * width_) + x;
    if ((visible_[index] & vision) != 0U) {
        return CellState::Visible;
    }
    return (explored_[index] & vision) != 0U ? CellState::Explored : CellState::Unexplored;
}

CellState FogOfWar::at(TeamId team, i64 x_raw, i64 z_raw) const noexcept {
    u32 x = 0;
    u32 z = 0;
    return locate(x_raw, z_raw, x, z) ? cell(team, x, z) : CellState::Unexplored;
}

UnitSight FogOfWar::sight(TeamId observer, u64 id) const noexcept {
    if (!valid_team(observer)) {
        return UnitSight::Hidden;
    }
    if (const u32* slot = seen_index_.find(id); slot != nullptr) {
        const Seen& seen = seen_[*slot];
        const bool allied = seen.team != kNoTeam && (vision_of(observer) & bit(seen.team)) != 0U;
        if (allied || (seen.seen_by & bit(observer)) != 0U) {
            return UnitSight::Visible;
        }
    }
    return ghost(observer, id) != nullptr ? UnitSight::Ghost : UnitSight::Hidden;
}

const FogGhost* FogOfWar::ghost(TeamId observer, u64 id) const noexcept {
    const usize index = ghost_index(observer, id);
    return index < ghosts_.size() ? &ghosts_[index] : nullptr;
}

Status FogOfWar::write_states(TeamId team, Span<u8> out) const noexcept {
    if (out.size() != static_cast<usize>(width_) * height_) {
        return fail(ErrorCode::InvalidArgument,
                    "fog of war: the state span is not width * height bytes");
    }
    const u32 vision = vision_of(team);
    for (usize index = 0; index < out.size(); ++index) {
        CellState state = CellState::Unexplored;
        if ((visible_[index] & vision) != 0U) {
            state = CellState::Visible;
        } else if ((explored_[index] & vision) != 0U) {
            state = CellState::Explored;
        }
        out[index] = static_cast<u8>(state);
    }
    return ok();
}

u64 FogOfWar::digest() const noexcept {
    u64 hash = kFnvOffset;
    hash = mix(hash, (static_cast<u64>(width_) << 32U) | height_);
    hash = mix(hash, statistics_.tick);
    for (const u32 share : share_) {
        hash = mix(hash, share);
    }
    for (usize cell = 0; cell < visible_.size(); ++cell) {
        hash = mix(hash, (static_cast<u64>(visible_[cell]) << 32U) | explored_[cell]);
    }
    for (const Seen& seen : seen_) {
        hash = mix(hash, seen.id);
        hash = mix(hash, (static_cast<u64>(seen.team) << 32U) | seen.seen_by);
    }
    for (const FogGhost& ghost : ghosts_) {
        hash = mix(hash, (static_cast<u64>(ghost.observer) << 32U) | ghost.owner);
        hash = mix(hash, ghost.id);
        hash = mix(hash, static_cast<u64>(ghost.x_raw));
        hash = mix(hash, static_cast<u64>(ghost.z_raw));
        hash = mix(hash, ghost.seen_tick);
    }
    return hash;
}

}  // namespace cy::gameplay::fog
