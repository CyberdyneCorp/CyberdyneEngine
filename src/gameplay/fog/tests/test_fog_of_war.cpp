// SPDX-License-Identifier: MIT
// unit.gameplay_fog — fog of war, the simulation half (src/gameplay/fog/).
//
// The grid is authoritative state, so every case here is about what a team may know: which cells
// it sees and has seen, which of another team's units it sees, and what it remembers. The last case
// holds a COMMITTED DIGEST: the same updates on x86-64 and arm64, Linux, macOS and Windows must end
// in the same bits, because two lockstep peers that disagreed about a cell would disagree about
// which orders are legal.

#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/fog/fog_of_war.h>
#include <cy/test/test.h>

#include <cstdio>

using namespace cy;
using namespace cy::gameplay::fog;
using cy::gameplay::kNoTeam;
using cy::gameplay::TeamId;

namespace {

constexpr TeamId kBlue = 1;
constexpr TeamId kRed = 2;
constexpr TeamId kGreen = 3;
/// What the patrol case ends in, on every platform. Recomputed only when the rule changes.
constexpr u64 kCommittedDigest = 0x1CB12D96ED6530ECULL;

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// Metres as Q32.32.
[[nodiscard]] constexpr i64 metres(i64 whole) noexcept {
    return whole * kOneMetreRaw;
}

/// A cell's centre on a grid of 1 m cells from the origin.
[[nodiscard]] constexpr i64 centre(i64 cell) noexcept {
    return metres(cell) + (kOneMetreRaw / 2);
}

[[nodiscard]] FogUnit unit(u64 id, TeamId team, i64 x_raw, i64 z_raw, i64 radius_raw,
                           bool ghost = false) noexcept {
    FogUnit made;
    made.id = id;
    made.team = team;
    made.x_raw = x_raw;
    made.z_raw = z_raw;
    made.reveal_radius_raw = radius_raw;
    made.leaves_ghost = ghost;
    return made;
}

[[nodiscard]] FogGridDescription grid(u32 width, u32 height) noexcept {
    FogGridDescription description;
    description.width = width;
    description.height = height;
    return description;
}

[[nodiscard]] u32 count(const FogOfWar& fog, TeamId team, CellState state) noexcept {
    u32 total = 0;
    for (u32 z = 0; z < fog.height(); ++z) {
        for (u32 x = 0; x < fog.width(); ++x) {
            total += fog.cell(team, x, z) == state ? 1U : 0U;
        }
    }
    return total;
}

}  // namespace

CY_TEST_CASE("fog: a unit sees the cells whose centres lie within its reveal radius") {
    FogOfWar fog(allocator());
    CY_REQUIRE(fog.create(grid(16, 16)).has_value());
    // At a cell centre with a radius of two cells: every (dx, dz) with dx^2 + dz^2 <= 4 — 13 cells.
    const FogUnit scout = unit(1, kBlue, centre(8), centre(8), metres(2));
    CY_REQUIRE(fog.update({&scout, 1}, 1).has_value());
    CY_CHECK_EQ(count(fog, kBlue, CellState::Visible), 13U);
    CY_CHECK(fog.cell(kBlue, 10, 8) == CellState::Visible);
    CY_CHECK(fog.cell(kBlue, 8, 6) == CellState::Visible);
    CY_CHECK(fog.cell(kBlue, 10, 10) == CellState::Unexplored);
    CY_CHECK(fog.cell(kBlue, 11, 8) == CellState::Unexplored);
    // The other team saw nothing.
    CY_CHECK_EQ(count(fog, kRed, CellState::Unexplored), 256U);
    CY_CHECK_EQ(fog.statistics().cells_visible_any, 13U);
}

CY_TEST_CASE("fog: what a team saw stays explored after the unit has gone") {
    FogOfWar fog(allocator());
    CY_REQUIRE(fog.create(grid(32, 8)).has_value());
    FogUnit scout = unit(1, kBlue, centre(4), centre(4), metres(2));
    CY_REQUIRE(fog.update({&scout, 1}, 1).has_value());
    scout.x_raw = centre(20);
    CY_REQUIRE(fog.update({&scout, 1}, 2).has_value());
    CY_CHECK(fog.cell(kBlue, 4, 4) == CellState::Explored);
    CY_CHECK(fog.cell(kBlue, 20, 4) == CellState::Visible);
    CY_CHECK_EQ(count(fog, kBlue, CellState::Visible), 13U);
    CY_CHECK_EQ(count(fog, kBlue, CellState::Explored), 13U);
    CY_CHECK(fog.at(kBlue, centre(4), centre(4)) == CellState::Explored);
    // Off the grid and a teamless observer are unexplored, not an error.
    CY_CHECK(fog.at(kBlue, metres(-3), centre(4)) == CellState::Unexplored);
    CY_CHECK(fog.cell(kNoTeam, 20, 4) == CellState::Unexplored);
}

CY_TEST_CASE("fog: a position left of the origin is located by flooring, not truncating") {
    // Truncation would put -0.5 m in cell 0 and a unit there would see cell 0 as its own.
    FogGridDescription description = grid(8, 8);
    description.origin_x_raw = metres(-4);
    description.origin_z_raw = metres(-4);
    FogOfWar fog(allocator());
    CY_REQUIRE(fog.create(description).has_value());
    u32 x = 99;
    u32 z = 99;
    CY_REQUIRE(fog.locate(-(kOneMetreRaw / 2), -(kOneMetreRaw / 2), x, z));
    CY_CHECK_EQ(x, 3U);
    CY_CHECK_EQ(z, 3U);
    CY_CHECK_FALSE(fog.locate(metres(-5), 0, x, z));
    // A unit off the grid still lights the cells its sight reaches on it.
    // A metre left of the grid, level with row 4's centres.
    const FogUnit outside = unit(1, kBlue, metres(-5), centre(4) - metres(4), metres(2));
    CY_REQUIRE(fog.update({&outside, 1}, 1).has_value());
    CY_CHECK(fog.cell(kBlue, 0, 4) == CellState::Visible);
    CY_CHECK(fog.cell(kBlue, 1, 4) == CellState::Unexplored);
}

CY_TEST_CASE("fog: allies that share vision see what either sees, and nobody else does") {
    FogOfWar fog(allocator());
    CY_REQUIRE(fog.create(grid(32, 8)).has_value());
    CY_REQUIRE(fog.share_vision(kBlue, kGreen).has_value());
    const FogUnit units[] = {unit(1, kBlue, centre(4), centre(4), metres(1)),
                             unit(2, kGreen, centre(20), centre(4), metres(1))};
    CY_REQUIRE(fog.update(units, 1).has_value());
    CY_CHECK(fog.cell(kBlue, 20, 4) == CellState::Visible);
    CY_CHECK(fog.cell(kGreen, 4, 4) == CellState::Visible);
    CY_CHECK(fog.cell(kRed, 4, 4) == CellState::Unexplored);
    CY_CHECK(fog.sight(kBlue, 2) == UnitSight::Visible);
    CY_CHECK_FALSE(fog.share_vision(kBlue, 33).has_value());
}

CY_TEST_CASE("fog: another team's unit is seen only inside vision, and a mobile one is forgotten") {
    FogOfWar fog(allocator());
    CY_REQUIRE(fog.create(grid(32, 8)).has_value());
    FogUnit units[] = {unit(1, kBlue, centre(4), centre(4), metres(3)),
                       unit(2, kRed, centre(6), centre(4), metres(3))};
    CY_REQUIRE(fog.update(units, 1).has_value());
    CY_CHECK(fog.sight(kBlue, 2) == UnitSight::Visible);
    CY_CHECK(fog.sight(kRed, 1) == UnitSight::Visible);
    CY_CHECK(fog.sight(kBlue, 1) == UnitSight::Visible);
    // The red unit walks out of blue's sight: hidden, and no ghost of a unit that moves.
    units[1].x_raw = centre(28);
    CY_REQUIRE(fog.update(units, 2).has_value());
    CY_CHECK(fog.sight(kBlue, 2) == UnitSight::Hidden);
    CY_CHECK(fog.ghost(kBlue, 2) == nullptr);
    CY_CHECK(fog.sight(kBlue, 77) == UnitSight::Hidden);
    CY_CHECK(fog.sight(kNoTeam, 1) == UnitSight::Hidden);
}

CY_TEST_CASE("fog: a structure seen once is a ghost where it was, until that place is seen again") {
    FogOfWar fog(allocator());
    CY_REQUIRE(fog.create(grid(32, 8)).has_value());
    FogUnit units[] = {unit(1, kBlue, centre(4), centre(4), metres(3)),
                       unit(2, kRed, centre(6), centre(4), metres(1), true)};
    CY_REQUIRE(fog.update(units, 1).has_value());
    CY_CHECK(fog.sight(kBlue, 2) == UnitSight::Visible);
    // Its own team never holds a ghost of it.
    CY_CHECK(fog.ghost(kRed, 2) == nullptr);

    // Blue's scout leaves: the barracks is remembered where it stood, at the tick it was seen.
    units[0].x_raw = centre(24);
    CY_REQUIRE(fog.update(units, 2).has_value());
    CY_CHECK(fog.sight(kBlue, 2) == UnitSight::Ghost);
    const FogGhost* ghost = fog.ghost(kBlue, 2);
    CY_REQUIRE(ghost != nullptr);
    CY_CHECK_EQ(ghost->x_raw, centre(6));
    CY_CHECK_EQ(ghost->seen_tick, 1U);
    CY_CHECK_EQ(ghost->owner, kRed);
    CY_CHECK_EQ(fog.statistics().ghosts, 1U);

    // Destroyed while nobody watched: still a ghost, because blue does not know.
    CY_REQUIRE(fog.update({units, 1}, 3).has_value());
    CY_CHECK(fog.sight(kBlue, 2) == UnitSight::Ghost);
    // Blue looks again and finds nothing there: the ghost is forgotten.
    units[0].x_raw = centre(5);
    CY_REQUIRE(fog.update({units, 1}, 4).has_value());
    CY_CHECK(fog.sight(kBlue, 2) == UnitSight::Hidden);
    CY_CHECK(fog.ghosts().empty());
}

CY_TEST_CASE("fog: a ghost seen again is refreshed rather than forgotten") {
    FogOfWar fog(allocator());
    CY_REQUIRE(fog.create(grid(32, 8)).has_value());
    FogUnit units[] = {unit(1, kBlue, centre(4), centre(4), metres(3)),
                       unit(5, kNoTeam, centre(6), centre(4), 0, true),
                       unit(9, kRed, centre(7), centre(4), 0, true)};
    CY_REQUIRE(fog.update(units, 1).has_value());
    units[0].x_raw = centre(24);
    CY_REQUIRE(fog.update(units, 2).has_value());
    units[0].x_raw = centre(4);
    CY_REQUIRE(fog.update(units, 3).has_value());
    CY_CHECK(fog.sight(kBlue, 5) == UnitSight::Visible);
    const FogGhost* mine = fog.ghost(kBlue, 5);
    CY_REQUIRE(mine != nullptr);
    CY_CHECK_EQ(mine->seen_tick, 3U);
    // Ordered by observer then id, whatever order they were first seen in.
    CY_REQUIRE_EQ(fog.ghosts().size(), 2U);
    CY_CHECK_EQ(fog.ghosts()[0].id, 5U);
    CY_CHECK_EQ(fog.ghosts()[1].id, 9U);
}

CY_TEST_CASE("fog: a refused update changes nothing") {
    FogOfWar fog(allocator());
    CY_REQUIRE(fog.create(grid(16, 16)).has_value());
    const FogUnit first = unit(1, kBlue, centre(8), centre(8), metres(2));
    CY_REQUIRE(fog.update({&first, 1}, 1).has_value());
    const u64 before = fog.digest();
    const FogUnit twins[] = {unit(1, kBlue, centre(2), centre(2), metres(2)),
                             unit(1, kRed, centre(12), centre(12), metres(2))};
    CY_CHECK_FALSE(fog.update(twins, 2).has_value());
    const FogUnit stranger = unit(3, 33, centre(2), centre(2), metres(2));
    CY_CHECK_FALSE(fog.update({&stranger, 1}, 2).has_value());
    CY_CHECK_EQ(fog.digest(), before);
    CY_CHECK(fog.sight(kBlue, 1) == UnitSight::Visible);
    CY_CHECK(fog.cell(kBlue, 2, 2) == CellState::Unexplored);
}

CY_TEST_CASE("fog: a grid out of range is refused") {
    FogOfWar fog(allocator());
    CY_CHECK_FALSE(fog.create(grid(0, 8)).has_value());
    CY_CHECK_FALSE(fog.create(grid(8, kMaxFogCells + 1)).has_value());
    FogGridDescription tiny = grid(8, 8);
    tiny.cell_size_raw = 1;
    CY_CHECK_FALSE(fog.create(tiny).has_value());
    CY_CHECK_FALSE(fog.created());
    const FogUnit scout = unit(1, kBlue, 0, 0, metres(1));
    CY_CHECK_FALSE(fog.update({&scout, 1}, 1).has_value());
}

CY_TEST_CASE("fog: the state bytes are the cells, row-major with x fastest") {
    FogOfWar fog(allocator());
    CY_REQUIRE(fog.create(grid(8, 4)).has_value());
    FogUnit scout = unit(1, kBlue, centre(1), centre(1), metres(1));
    CY_REQUIRE(fog.update({&scout, 1}, 1).has_value());
    scout.x_raw = centre(6);
    CY_REQUIRE(fog.update({&scout, 1}, 2).has_value());
    u8 states[32] = {};
    CY_REQUIRE(fog.write_states(kBlue, states).has_value());
    for (u32 z = 0; z < 4U; ++z) {
        for (u32 x = 0; x < 8U; ++x) {
            CY_CHECK_EQ(states[(z * 8U) + x], static_cast<u8>(fog.cell(kBlue, x, z)));
        }
    }
    CY_CHECK_EQ(states[(1U * 8U) + 6U], static_cast<u8>(CellState::Visible));
    CY_CHECK_EQ(states[(1U * 8U) + 1U], static_cast<u8>(CellState::Explored));
    u8 short_span[31] = {};
    CY_CHECK_FALSE(fog.write_states(kBlue, short_span).has_value());
}

CY_TEST_CASE("fog: two peers fed the same updates agree, and a committed digest pins the bits") {
    // A patrol of sixteen units of three teams, a structure each, on a 32 x 32 grid of 3 m cells,
    // over 40 ticks of integer motion. The digest is computed on every CI leg.
    FogGridDescription description = grid(32, 32);
    description.cell_size_raw = kOneMetreRaw * 3;
    description.origin_x_raw = metres(-48);
    description.origin_z_raw = metres(-48);
    FogOfWar a(allocator());
    FogOfWar b(allocator());
    CY_REQUIRE(a.create(description).has_value());
    CY_REQUIRE(b.create(description).has_value());
    CY_REQUIRE(a.share_vision(kBlue, kGreen).has_value());
    CY_REQUIRE(b.share_vision(kBlue, kGreen).has_value());
    FogUnit units[19];
    for (u32 tick = 1; tick <= 40U; ++tick) {
        for (u32 index = 0; index < 16U; ++index) {
            const i64 phase = static_cast<i64>((tick * (index + 3U)) % 97U);
            const TeamId team = static_cast<TeamId>(1U + (index % 3U));
            units[index] = unit(100 + index, team, metres(-40) + (phase * (kOneMetreRaw * 4) / 5),
                                metres(-40 + static_cast<i64>(index) * 5) + (phase << 20),
                                metres(4) + (static_cast<i64>(index) << 28));
        }
        for (u32 team = 0; team < 3U; ++team) {
            units[16 + team] =
                unit(900 + team, static_cast<TeamId>(team + 1U),
                     metres(-30 + static_cast<i64>(team) * 30), metres(10), metres(2), true);
        }
        CY_REQUIRE(a.update(units, tick).has_value());
        CY_REQUIRE(b.update(units, tick).has_value());
        if (tick % 10U == 0U) {
            CY_REQUIRE_EQ(a.digest(), b.digest());
        }
    }
    std::printf("gameplay_fog digest=0x%016llx ghosts=%u visible=%u\n",
                static_cast<unsigned long long>(a.digest()), a.statistics().ghosts,
                a.statistics().cells_visible_any);
    CY_CHECK_EQ(a.digest(), kCommittedDigest);
    // One peer whose scout stood three metres elsewhere on the next tick no longer agrees.
    CY_REQUIRE(a.update(units, 41).has_value());
    units[3].x_raw += metres(3);
    CY_REQUIRE(b.update(units, 41).has_value());
    CY_CHECK_NE(a.digest(), b.digest());
}
