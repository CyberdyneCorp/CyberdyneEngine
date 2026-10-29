// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_API_BATTLE_H
#define CY_SAMPLE_RTS_API_BATTLE_H
// battle.h — the battle of samples/13-rts-api (`--battle`), and its C++ twin. ABI 1.16,
// openspec/changes/add-rts-combat-and-minimap.
//
// THE BATTLE. On the lockstep company's field (company.h), south of its rock outcrop, blue's army —
// swordsmen, archers and catapults — and red's — swordsmen and archers, before a watchtower and a
// barracks — attack each other. The SWIFT COMMANDER (game/Battle.swift) enlists and arms every
// unit, places red's two buildings and gives every order, through `Lockstep.enlist`,
// `Lockstep.arm`, `Lockstep.fortify` and `Lockstep.order` (`kPlan`): both armies attack-move at
// each other on tick 0; on tick 540 blue's soldiers push on while its catapults turn on the
// watchtower; on tick 720 the soldiers join them, and on tick 960 all of blue goes for the
// barracks. Every number is a `Fixed` or an integer, so the battle is bits: who struck whom, every
// point of damage, every projectile's flight, every death and the fall of both buildings.
//
// THE TWIN. `run_battle()` runs the same session with the same units, the same weapons and the
// same orders, from C++. `determinism.cross_leg` publishes its digest as `detmath-battle-digest`
// on all four CI legs, each of which checks it against `kBattleDigest`, and `cross-leg-compare`
// compares the legs; `integration.rts_api_sample` holds the Swift-driven battle to the same
// committed number. So the battle the Swift game fights is the C++ one, bit for bit, on x86-64 and
// arm64, Linux, macOS and Windows.
//
// Header-only and free of the sample's host, because tests/determinism/test_cross_leg.cpp includes
// it, as it does company.h.

#include <cy/core/base/types.h>
#include <cy/core/detmath/fixed.h>
#include <cy/core/detmath/vec.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/lockstep_session.h>
#include <cy/navigation/navmesh.h>

#include <memory>

#include "company.h"

namespace sample::rts::battle {

using cy::detmath::Fixed;
using cy::detmath::FixedVec2;
using cy::game_backend::LockstepCombatSpec;
using cy::game_backend::LockstepWeapon;

/// The ticks a battle run lasts: one a frame, twenty seconds.
inline constexpr cy::u64 kTicks = 1200;
/// Where the field lies in the level's metres: the RTS test map's `company` placement, which
/// content/rts/frontier.cymap states and integration.rts_map holds the cooked map to.
inline constexpr cy::f32 kFieldX = -8.0F;
inline constexpr cy::f32 kFieldZ = -52.0F;
/// The RTS test map's terrain, which the fog of war covers in a battle whether or not the map is
/// drawn: 128 m from (-48, -64).
inline constexpr cy::f32 kMapMinX = -48.0F;
inline constexpr cy::f32 kMapMinZ = -64.0F;
inline constexpr cy::f32 kMapSize = 128.0F;

/// The fog of war's teams (game/Fog.swift's `BaseFog`): the armies are blue's and red's.
inline constexpr cy::u32 kBlue = 1;
inline constexpr cy::u32 kRed = 2;

/// What each kind of soldier is. `Battle.Kind` in game/Battle.swift, case for case.
enum class Kind : cy::u32 { BlueSword = 0, BlueArcher, Catapult, RedSword, RedArcher, Count };

/// A squad: its kind, its lockstep group, how many, and its block — first unit's place, units a
/// row, spacing in quarter metres.
struct Squad {
    Kind kind;
    cy::u32 group;
    cy::u32 count;
    cy::i32 x_quarters;
    cy::i32 z_quarters;
    cy::u32 per_row;
    cy::i32 spacing_quarters;
};

/// The armies in enlistment order, which is the units' identity in the session.
inline constexpr Squad kSquads[] = {
    {Kind::BlueSword, 0, 10, 12 * 4, 4 * 4, 5, 9}, {Kind::BlueArcher, 1, 10, 5 * 4, 4 * 4, 4, 9},
    {Kind::Catapult, 2, 4, 3 * 4, 13 * 4, 4, 12},  {Kind::RedSword, 3, 8, 30 * 4, 5 * 4, 4, 9},
    {Kind::RedArcher, 4, 8, 37 * 4, 10 * 4, 4, 9},
};
inline constexpr cy::u32 kSquadCount = sizeof(kSquads) / sizeof(kSquads[0]);

/// Units in the battle: 40.
[[nodiscard]] constexpr cy::u32 unit_count() noexcept {
    cy::u32 total = 0;
    for (const Squad& squad : kSquads) {
        total += squad.count;
    }
    return total;
}
inline constexpr cy::u32 kUnits = unit_count();

/// The quarter metres `q` as a `Fixed`, exactly.
[[nodiscard]] constexpr Fixed quarters(cy::i32 q) noexcept {
    return Fixed::from_raw(static_cast<cy::i64>(q) * (Fixed::kOneRaw / 4));
}

/// Where unit `index` of `squad` starts.
[[nodiscard]] constexpr FixedVec2 spawn(const Squad& squad, cy::u32 index) noexcept {
    const auto column = static_cast<cy::i32>(index % squad.per_row);
    const auto row = static_cast<cy::i32>(index / squad.per_row);
    return FixedVec2{quarters(squad.x_quarters + (column * squad.spacing_quarters)),
                     quarters(squad.z_quarters + (row * squad.spacing_quarters))};
}

/// The team a kind fights for.
[[nodiscard]] constexpr cy::u32 team_of(Kind kind) noexcept {
    return kind == Kind::RedSword || kind == Kind::RedArcher ? kRed : kBlue;
}

/// A kind's weapon and health. `Battle.arms(_:)` in game/Battle.swift.
[[nodiscard]] inline LockstepCombatSpec arms(Kind kind) noexcept {
    LockstepCombatSpec spec;
    spec.team = team_of(kind);
    switch (kind) {
        case Kind::BlueSword:
        case Kind::RedSword:
            spec.health = kind == Kind::BlueSword ? 120 : 130;
            spec.armour = kind == Kind::BlueSword ? 3 : 2;
            spec.weapon = LockstepWeapon::Melee;
            spec.damage = kind == Kind::BlueSword ? 16 : 18;
            spec.cooldown = 36;
            // A reach past the crowd's separation (2 m between two half-metre units' centres), so
            // two fighters standing at it are not pushed out of it.
            spec.range = quarters(5);
            spec.sight = Fixed::from_int(8);
            break;
        case Kind::BlueArcher:
        case Kind::RedArcher:
            spec.health = kind == Kind::BlueArcher ? 70 : 75;
            spec.weapon = LockstepWeapon::Ranged;
            spec.damage = kind == Kind::BlueArcher ? 11 : 13;
            spec.cooldown = 54;
            spec.range = Fixed::from_int(kind == Kind::BlueArcher ? 11 : 10);
            spec.sight = Fixed::from_int(13);
            spec.projectile_speed = Fixed::from_int(22);
            break;
        case Kind::Catapult:
            spec.health = 150;
            spec.armour = 2;
            spec.weapon = LockstepWeapon::Ranged;
            spec.damage = 30;
            spec.cooldown = 150;
            spec.range = Fixed::from_int(20);
            spec.sight = Fixed::from_int(21);
            spec.projectile_speed = Fixed::from_int(15);
            spec.splash = quarters(10);
            break;
        case Kind::Count:
            break;
    }
    return spec;
}

/// A catapult is bigger and slower than a soldier; zero is the mover's default for the rest.
[[nodiscard]] constexpr Fixed radius_of(Kind kind) noexcept {
    return kind == Kind::Catapult ? quarters(4) : Fixed::zero();
}
[[nodiscard]] constexpr Fixed speed_of(Kind kind) noexcept {
    return kind == Kind::Catapult ? quarters(12) : Fixed::zero();
}

/// Red's two buildings, in fortification order.
struct Building {
    cy::i32 x_quarters;
    cy::i32 z_quarters;
    cy::i32 radius_quarters;
    cy::u32 health;
    cy::u32 armour;
};
inline constexpr Building kBuildings[] = {
    {44 * 4, 4 * 4, 10, 400, 3},   // the watchtower
    {42 * 4, 16 * 4, 12, 400, 4},  // the barracks
};
inline constexpr cy::u32 kBuildingCount = 2;

/// One order of the battle's plan.
struct PlannedOrder {
    cy::u64 tick;
    cy::game_backend::LockstepOrderKind kind;
    cy::u32 group;
    cy::i32 x_quarters;
    cy::i32 z_quarters;
};

/// The plan, in the order the commander records it on each tick.
inline constexpr PlannedOrder kPlan[] = {
    {0, cy::game_backend::LockstepOrderKind::AttackMove, 0, 34 * 4, 7 * 4},
    {0, cy::game_backend::LockstepOrderKind::AttackMove, 1, 26 * 4, 7 * 4},
    {0, cy::game_backend::LockstepOrderKind::AttackMove, 2, 16 * 4, 10 * 4},
    {0, cy::game_backend::LockstepOrderKind::AttackMove, 3, 14 * 4, 6 * 4},
    {0, cy::game_backend::LockstepOrderKind::AttackMove, 4, 22 * 4, 9 * 4},
    {540, cy::game_backend::LockstepOrderKind::AttackMove, 0, 40 * 4, 9 * 4},
    {540, cy::game_backend::LockstepOrderKind::AttackMove, 1, 36 * 4, 9 * 4},
    {540, cy::game_backend::LockstepOrderKind::Attack, 2, 44 * 4, 4 * 4},
    {720, cy::game_backend::LockstepOrderKind::Attack, 0, 44 * 4, 4 * 4},
    {720, cy::game_backend::LockstepOrderKind::Attack, 1, 44 * 4, 4 * 4},
    {960, cy::game_backend::LockstepOrderKind::Attack, 0, 42 * 4, 16 * 4},
    {960, cy::game_backend::LockstepOrderKind::Attack, 1, 42 * 4, 16 * 4},
    {960, cy::game_backend::LockstepOrderKind::Attack, 2, 42 * 4, 16 * 4},
};

/// What a battle run ends in: the digest and the battle's tally.
struct BattleRun {
    cy::u64 digest = 0;
    cy::u64 final_hash = 0;
    cy::u64 ticks = 0;
    cy::u64 events = 0;
    cy::u32 units = 0;
    cy::u32 commands = 0;
    cy::u32 disagreements = 0;
    cy::u32 blue_alive = 0;
    cy::u32 red_alive = 0;
    cy::u32 buildings_standing = 0;
    /// Points dealt, by every hit of the battle.
    cy::u64 damage = 0;
    bool complete = false;
};

/// Enlist, arm and fortify the battle in `session`, as game/Battle.swift does through the ABI.
[[nodiscard]] inline bool set_up(cy::game_backend::LockstepSession& session) noexcept {
    for (const Squad& squad : kSquads) {
        for (cy::u32 index = 0; index < squad.count; ++index) {
            cy::game_backend::LockstepUnitSpec unit;
            unit.group = squad.group;
            unit.position = spawn(squad, index);
            unit.radius = radius_of(squad.kind);
            unit.max_speed = speed_of(squad.kind);
            auto enlisted = session.enlist(unit);
            if (!enlisted || !session.arm(*enlisted, arms(squad.kind))) {
                return false;
            }
        }
    }
    for (const Building& building : kBuildings) {
        cy::game_backend::LockstepStructureSpec spec;
        spec.team = kRed;
        spec.position = FixedVec2{quarters(building.x_quarters), quarters(building.z_quarters)};
        spec.radius = quarters(building.radius_quarters);
        spec.health = building.health;
        spec.armour = building.armour;
        if (!session.fortify(spec)) {
            return false;
        }
    }
    return true;
}

/// Record the plan's orders for `tick`.
[[nodiscard]] inline bool give_orders(cy::game_backend::LockstepSession& session,
                                      cy::u64 tick) noexcept {
    for (const PlannedOrder& planned : kPlan) {
        if (planned.tick != tick) {
            continue;
        }
        cy::game_backend::LockstepOrderPayload order;
        order.kind = static_cast<cy::u32>(planned.kind);
        order.group = planned.group;
        order.target_x = quarters(planned.x_quarters).raw;
        order.target_z = quarters(planned.z_quarters).raw;
        if (!session.record(order)) {
            return false;
        }
    }
    return true;
}

/// The battle's digest over `ticks`, run by C++ alone: an issuer and a follower driven by its log.
/// `issue_orders` false is the control: the same armies, never ordered, never meet.
[[nodiscard]] inline BattleRun run_battle(cy::u64 ticks = kTicks,
                                          bool issue_orders = true) noexcept {
    BattleRun run;
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::World);
    cy::navigation::NavMesh baked(allocator, cy::Name::intern("rts.company"),
                                  static_cast<cy::f32>(company::kCells * company::kCellMetres));
    if (!company::bake_field(baked)) {
        return run;
    }
    auto issuer = std::make_unique<cy::game_backend::LockstepSession>(allocator, company::config());
    auto follower =
        std::make_unique<cy::game_backend::LockstepSession>(allocator, company::config());
    if (!issuer->load(baked) || !follower->load(baked) || !set_up(*issuer) || !set_up(*follower)) {
        return run;
    }
    cy::u32 cursor = 0;
    for (cy::u64 tick = 0; tick < ticks; ++tick) {
        if (issue_orders && !give_orders(*issuer, tick)) {
            return run;
        }
        if (!issuer->advance() || !follower->receive(issuer->log(), cursor) ||
            !follower->advance()) {
            return run;
        }
        run.disagreements += issuer->state_hash() == follower->state_hash() ? 0U : 1U;
        for (const cy::game_backend::LockstepEvent& event : issuer->events()) {
            run.damage +=
                event.kind == cy::game_backend::LockstepEventKind::Damage ? event.amount : 0U;
        }
    }
    for (cy::u32 unit = 0; unit < issuer->units(); ++unit) {
        auto state = issuer->combatant(unit);
        if (state && state->alive) {
            run.blue_alive += state->team == kBlue ? 1U : 0U;
            run.red_alive += state->team == kRed ? 1U : 0U;
        }
    }
    for (cy::u32 index = 0; index < issuer->structures(); ++index) {
        auto state = issuer->structure(index);
        run.buildings_standing += state && state->standing ? 1U : 0U;
    }
    run.digest = issuer->digest();
    run.final_hash = issuer->state_hash();
    run.ticks = issuer->tick();
    run.events = issuer->event_count();
    run.units = issuer->units();
    run.commands = issuer->log().size();
    run.complete = true;
    return run;
}

/// The digest `run_battle()` computes, committed: every leg's, and the Swift-driven sample's.
inline constexpr cy::u64 kBattleDigest = 0x310f'cf0d'efc6'57b8ULL;

}  // namespace sample::rts::battle

#endif  // CY_SAMPLE_RTS_API_BATTLE_H
