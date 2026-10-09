// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_API_COMPANY_H
#define CY_SAMPLE_RTS_API_COMPANY_H
// company.h — the lockstep company of samples/13-rts-api, and its C++ twin. ABI 1.8,
// openspec/changes/add-deterministic-math stage 8.
//
// THE COMPANY. Two groups of eight units on a field of their own, a fixed-point world the host runs
// as a lockstep session (cy::game_backend::LockstepSession, with a follower peer driven by the
// issuer's command log alone). The SWIFT COMMANDER enlists the units and gives the orders, through
// `Lockstep.enlist` and `Lockstep.order`: every `kOrderEvery` ticks each group is sent to the next
// waypoint of a patrol round the field, a point it computes in `Fixed` — the waypoint's angle is
// integer arithmetic on a binary angle, its cosine and sine are `Detmath.cos`/`Detmath.sin`
// (ABI 1.8, the engine's polynomials), and the point is `centre + (cos, sin) * radius` with
// CyberdyneKit's `Fixed` `*` and `+`. So the orders a Swift game issues are bits, and every peer,
// on every architecture, executes those bits.
//
// THE TWIN. `run_company()` below runs the same session with the same orders computed in C++ from
// the same formulas. `determinism.cross_leg` publishes its digest as `detmath-company-digest` on
// all four CI legs, where each leg checks it against `kCompanyDigest` and `cross-leg-compare`
// compares the legs; `integration.rts_api_sample` checks that the digest the SWIFT-driven sample
// reports is that same committed number. The three together say: Swift's fixed-point orders are
// C++'s, bit for bit, and the session they drive is the same on x86-64 and arm64, Linux, macOS and
// Windows.
//
// Header-only and free of the sample's host, because tests/determinism/test_cross_leg.cpp includes
// it. Every float here is the field's bake, which a cooked asset would hold; it is converted once,
// in `LockstepSession::load`, before the first tick.

#include <cy/core/base/types.h>
#include <cy/core/detmath/fixed.h>
#include <cy/core/detmath/functions.h>
#include <cy/core/detmath/vec.h>
#include <cy/core/memory/hash.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/lockstep_session.h>
#include <cy/navigation/navmesh.h>

#include <memory>
#include <utility>

namespace sample::rts::company {

using cy::detmath::Fixed;
using cy::detmath::FixedVec2;

/// Quads per side of the field; each is `kCellMetres` on a side.
inline constexpr cy::u32 kCells = 24;
inline constexpr cy::i32 kCellMetres = 2;
/// Groups, and units in each.
inline constexpr cy::u32 kGroups = 2;
inline constexpr cy::u32 kGroupSize = 8;
/// An order to every group every this many ticks, from tick 0.
inline constexpr cy::u64 kOrderEvery = 90;
/// The ticks the sample runs: one per frame, `Player::kFrames` of them (script.h).
inline constexpr cy::u64 kTicks = 420;
/// The patrol: a circle of `kRadius` metres about the field's centre, `kWaypointTurn` of a turn
/// further round at each order, the second group half a turn behind the first.
inline constexpr cy::i32 kCentre = 24;
inline constexpr cy::i32 kRadius = 14;
inline constexpr cy::u32 kWaypointTurn = 0x3333'3333U;  // a fifth of a turn, truncated
inline constexpr cy::u32 kGroupTurn = 0x8000'0000U;     // half a turn

/// The session's configuration: the defaults, and a seed of the sample's own.
[[nodiscard]] inline cy::game_backend::LockstepConfig config() noexcept {
    cy::game_backend::LockstepConfig config;
    config.seed = 0x13'4757'0008ULL;
    return config;
}

/// Whether quad (`row`, `column`) of the field is walkable: a pillar block off the centre.
[[nodiscard]] constexpr bool walkable(cy::u32 row, cy::u32 column) noexcept {
    return row < 9 || row >= 12 || column < 13 || column >= 16;
}

/// The field, baked: what a cook would have written, in float, as a cooked asset holds it.
[[nodiscard]] inline bool bake_field(cy::navigation::NavMesh& mesh) noexcept {
    cy::navigation::NavTileData data(mesh.allocator());
    data.coord = cy::navigation::TileCoord{0, 0, 0};
    for (cy::u32 row = 0; row <= kCells; ++row) {
        for (cy::u32 column = 0; column <= kCells; ++column) {
            if (!data.vertices().push_back(cy::Vec3{static_cast<cy::f32>(column * kCellMetres),
                                                    0.0F,
                                                    static_cast<cy::f32>(row * kCellMetres)})) {
                return false;
            }
        }
    }
    for (cy::u32 row = 0; row < kCells; ++row) {
        for (cy::u32 column = 0; column < kCells; ++column) {
            if (!walkable(row, column)) {
                continue;
            }
            cy::navigation::NavPoly poly;
            poly.first_corner = static_cast<cy::u32>(data.corners().size());
            poly.corner_count = 4;
            if (!data.polys().push_back(poly)) {
                return false;
            }
            const cy::u32 base = row * (kCells + 1);
            const cy::u32 corner[4] = {base + column, base + column + 1,
                                       base + kCells + 1 + column + 1, base + kCells + 1 + column};
            for (const cy::u32 index : corner) {
                if (!data.corners().push_back(index)) {
                    return false;
                }
            }
        }
    }
    data.finalise();
    return mesh.add_tile(std::move(data)).has_value();
}

/// Where unit `index` of `group` starts: a 4 x 2 block, 2.25 m apart, the first group in the
/// field's south-west, the second in its north-east. `CompanyCommander` in Commander.swift places
/// them by the same formula.
[[nodiscard]] inline FixedVec2 spawn(cy::u32 group, cy::u32 index) noexcept {
    const Fixed spacing = Fixed::from_raw((Fixed::kOneRaw * 9) / 4);
    const Fixed origin = Fixed::from_int(group == 0 ? 4 : 36);
    return FixedVec2{origin + Fixed::from_int(static_cast<cy::i32>(index % 4)) * spacing,
                     origin + Fixed::from_int(static_cast<cy::i32>(index / 4)) * spacing};
}

/// The waypoint `group` is sent to by the order of `tick` (a multiple of `kOrderEvery`).
[[nodiscard]] inline FixedVec2 waypoint(cy::u64 tick, cy::u32 group) noexcept {
    const auto order = static_cast<cy::u32>(tick / kOrderEvery);
    const cy::detmath::Angle angle =
        cy::detmath::Angle::from_raw((order * kWaypointTurn) + (group * kGroupTurn));
    const cy::detmath::SinCos direction = cy::detmath::sincos(angle);
    const Fixed centre = Fixed::from_int(kCentre);
    const Fixed radius = Fixed::from_int(kRadius);
    return FixedVec2{centre + (direction.cos * radius), centre + (direction.sin * radius)};
}

/// The company's digest over `ticks`, run by C++ alone: an issuer and a follower driven by its log.
struct CompanyRun {
    cy::u64 digest = 0;
    cy::u64 final_hash = 0;
    cy::u64 ticks = 0;
    cy::u32 units = 0;
    cy::u32 commands = 0;
    cy::u32 disagreements = 0;
    bool complete = false;
};

/// `issue_orders` false is the negative control: the same session given no orders.
[[nodiscard]] inline CompanyRun run_company(cy::u64 ticks = kTicks,
                                            bool issue_orders = true) noexcept {
    CompanyRun run;
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::World);
    cy::navigation::NavMesh baked(allocator, cy::Name::intern("rts.company"),
                                  static_cast<cy::f32>(kCells * kCellMetres));
    if (!bake_field(baked)) {
        return run;
    }
    auto issuer = std::make_unique<cy::game_backend::LockstepSession>(allocator, config());
    auto follower = std::make_unique<cy::game_backend::LockstepSession>(allocator, config());
    if (!issuer->load(baked) || !follower->load(baked)) {
        return run;
    }
    for (cy::u32 group = 0; group < kGroups; ++group) {
        for (cy::u32 index = 0; index < kGroupSize; ++index) {
            cy::game_backend::LockstepUnitSpec spec;
            spec.group = group;
            spec.position = spawn(group, index);
            if (!issuer->enlist(spec) || !follower->enlist(spec)) {
                return run;
            }
        }
    }
    cy::u32 cursor = 0;
    for (cy::u64 tick = 0; tick < ticks; ++tick) {
        if (issue_orders && tick % kOrderEvery == 0) {
            for (cy::u32 group = 0; group < kGroups; ++group) {
                const FixedVec2 target = waypoint(tick, group);
                cy::game_backend::LockstepOrderPayload order;
                order.kind = static_cast<cy::u32>(cy::game_backend::LockstepOrderKind::Move);
                order.group = group;
                order.target_x = target.x.raw;
                order.target_z = target.y.raw;
                if (!issuer->record(order)) {
                    return run;
                }
            }
        }
        if (!issuer->advance() || !follower->receive(issuer->log(), cursor) ||
            !follower->advance()) {
            return run;
        }
        run.disagreements += issuer->state_hash() == follower->state_hash() ? 0U : 1U;
    }
    run.digest = issuer->digest();
    run.final_hash = issuer->state_hash();
    run.ticks = issuer->tick();
    run.units = issuer->units();
    run.commands = issuer->log().size();
    run.complete = true;
    return run;
}

/// The digest `run_company()` computes, committed: every leg's, and the Swift-driven sample's.
inline constexpr cy::u64 kCompanyDigest = 0xa61e'5414'c179'f54aULL;

}  // namespace sample::rts::company

#endif  // CY_SAMPLE_RTS_API_COMPANY_H
