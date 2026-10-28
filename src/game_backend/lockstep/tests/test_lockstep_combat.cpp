// SPDX-License-Identifier: MIT
// `integration.game_backend_lockstep`, the combat half: health, weapons, projectiles, splash,
// structures and the two attack orders, in a real session, and through ABI 1.16's table.
// openspec/changes/add-rts-combat-and-minimap.
//
// THE NUMBERS ARE THE SPECS'. Every case states the health, armour, damage and cooldown it arms
// with, works out by hand the hits, the points and the tick each lands on, and holds the session to
// them — so a change to how damage, armour, cooldowns, flight or splash are computed is a red case
// naming the number that moved.
//
// The map is src/movement/tests/rts_scenario.h's: 2 m quads over 64 m, open along its southern
// strip (z below 16 m), where every case stands its units.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/lockstep_backend.h>
#include <cy/test/test.h>

#include <memory>
#include <vector>

#include "rts_scenario.h"

using namespace cy;
using cy::detmath::Fixed;
using cy::detmath::FixedVec2;
using cy::game_backend::LockstepAdapter;
using cy::game_backend::LockstepCombatSpec;
using cy::game_backend::LockstepConfig;
using cy::game_backend::LockstepEvent;
using cy::game_backend::LockstepEventKind;
using cy::game_backend::LockstepOrderKind;
using cy::game_backend::LockstepOrderPayload;
using cy::game_backend::LockstepSession;
using cy::game_backend::LockstepStructureSpec;
using cy::game_backend::LockstepTargetKind;
using cy::game_backend::LockstepUnitSpec;
using cy::game_backend::LockstepWeapon;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

constexpr u32 kMapCells = 32;
constexpr i64 kOne = Fixed::kOneRaw;

/// `metres` tenths of a metre, exactly.
[[nodiscard]] Fixed tenths(i32 value) noexcept {
    return Fixed::from_raw((static_cast<i64>(value) * kOne) / 10);
}

[[nodiscard]] FixedVec2 at(i32 x_tenths, i32 z_tenths) noexcept {
    return FixedVec2{tenths(x_tenths), tenths(z_tenths)};
}

[[nodiscard]] LockstepCombatSpec melee(u32 team, u32 health, u32 armour, u32 damage,
                                       u32 cooldown) noexcept {
    LockstepCombatSpec spec;
    spec.team = team;
    spec.health = health;
    spec.armour = armour;
    spec.weapon = LockstepWeapon::Melee;
    spec.damage = damage;
    spec.cooldown = cooldown;
    spec.range = tenths(12) + Fixed::from_raw(Fixed::kOneRaw / 20);  // 1.25 m
    spec.sight = Fixed::from_int(4);
    return spec;
}

/// A target that never notices anything: it fights only what touches it.
[[nodiscard]] LockstepCombatSpec dummy(u32 team, u32 health) noexcept {
    LockstepCombatSpec spec = melee(team, health, 0, 1, 1000);
    spec.range = Fixed::zero();
    spec.sight = Fixed::zero();
    return spec;
}

[[nodiscard]] LockstepCombatSpec archer(u32 team, u32 damage, u32 cooldown, i32 speed,
                                        Fixed splash = Fixed::zero()) noexcept {
    LockstepCombatSpec spec;
    spec.team = team;
    spec.health = 60;
    spec.weapon = LockstepWeapon::Ranged;
    spec.damage = damage;
    spec.cooldown = cooldown;
    spec.range = Fixed::from_int(10);
    spec.sight = Fixed::from_int(12);
    spec.projectile_speed = Fixed::from_int(speed);
    spec.splash = splash;
    return spec;
}

/// One session on the scenario's map, and the events every tick made, kept.
struct Battle {
    navigation::NavMesh baked{allocator(), Name::intern("lockstep.combat"), 64.0F};
    std::unique_ptr<LockstepSession> session;
    std::vector<LockstepEvent> events;

    explicit Battle(const LockstepConfig& config = LockstepConfig{}) noexcept {
        CY_REQUIRE(movement_test::bake_rts_mesh(kMapCells, baked));
        session = std::make_unique<LockstepSession>(allocator(), config);
        CY_REQUIRE(session->load(baked).has_value());
    }

    u32 unit(u32 group, FixedVec2 position, const LockstepCombatSpec& spec) noexcept {
        LockstepUnitSpec unit;
        unit.group = group;
        unit.position = position;
        auto enlisted = session->enlist(unit);
        CY_REQUIRE(enlisted.has_value());
        CY_REQUIRE(session->arm(*enlisted, spec).has_value());
        return *enlisted;
    }

    void order(LockstepOrderKind kind, u32 group, FixedVec2 target) noexcept {
        LockstepOrderPayload payload;
        payload.kind = static_cast<u32>(kind);
        payload.group = group;
        payload.target_x = target.x.raw;
        payload.target_z = target.y.raw;
        CY_REQUIRE(session->record(payload).has_value());
    }

    void run(u32 ticks) noexcept {
        for (u32 tick = 0; tick < ticks; ++tick) {
            CY_REQUIRE(session->advance().has_value());
            for (const LockstepEvent& event : session->events()) {
                events.push_back(event);
            }
        }
    }

    [[nodiscard]] std::vector<LockstepEvent> of(LockstepEventKind kind) const {
        std::vector<LockstepEvent> found;
        for (const LockstepEvent& event : events) {
            if (event.kind == kind) {
                found.push_back(event);
            }
        }
        return found;
    }

    [[nodiscard]] u32 health(u32 unit) const noexcept {
        auto state = session->combatant(unit);
        CY_REQUIRE(state.has_value());
        return state->health;
    }

    [[nodiscard]] bool alive(u32 unit) const noexcept {
        auto state = session->combatant(unit);
        CY_REQUIRE(state.has_value());
        return state->alive;
    }
};

}  // namespace

CY_TEST_CASE("combat: a melee duel's blows, points and death are the ones its specs give") {
    // A (index 0) deals 12 every 10 ticks through no armour; B deals 7 every 15 through A's 2,
    // so 5. They stand 2.1 m apart — a 1.1 m gap, within both 1.25 m reaches and beyond the
    // crowd's 2 m separation, which would otherwise push them apart — and neither moves.
    Battle battle;
    const u32 a = battle.unit(0, at(100, 60), melee(1, 50, 2, 12, 10));
    const u32 b = battle.unit(1, at(121, 60), melee(2, 40, 0, 7, 15));
    battle.run(40);

    // B: 40 - 12 (tick 0) - 12 (10) - 12 (20) = 4, and the fourth blow takes the last 4 (30).
    // A: 50 - 5 (tick 0) - 5 (15) = 40; B's blow due on tick 30 never comes, A struck first.
    const std::vector<LockstepEvent> damage = battle.of(LockstepEventKind::Damage);
    CY_REQUIRE_EQ(damage.size(), 6U);
    const struct {
        u64 tick;
        u32 attacker;
        u32 amount;
        u32 left;
    } expected[] = {{0, a, 12, 28}, {0, b, 5, 45},  {10, a, 12, 16},
                    {15, b, 5, 40}, {20, a, 12, 4}, {30, a, 4, 0}};
    for (usize index = 0; index < damage.size(); ++index) {
        CY_CHECK_EQ(damage[index].tick, expected[index].tick);
        CY_CHECK_EQ(damage[index].unit, expected[index].attacker);
        CY_CHECK_EQ(damage[index].amount, expected[index].amount);
        CY_CHECK_EQ(damage[index].health, expected[index].left);
    }
    CY_CHECK_EQ(battle.of(LockstepEventKind::Strike).size(), 6U);
    const std::vector<LockstepEvent> deaths = battle.of(LockstepEventKind::Death);
    CY_REQUIRE_EQ(deaths.size(), 1U);
    CY_CHECK_EQ(deaths[0].unit, b);
    CY_CHECK_EQ(deaths[0].tick, 30U);
    CY_CHECK_EQ(deaths[0].team, 2U);
    CY_CHECK_EQ(battle.health(a), 40U);
    CY_CHECK(battle.alive(a));
    CY_CHECK(!battle.alive(b));
    auto winner = battle.session->combatant(a);
    CY_REQUIRE(winner.has_value());
    CY_CHECK_EQ(winner->kills, 1U);
    CY_CHECK(!winner->target.some());
    // The dead leave the crowd: B's slot stays, inactive.
    CY_CHECK(!battle.session->mover().active(b));
    // Every event is numbered once, in order.
    for (usize index = 0; index < battle.events.size(); ++index) {
        CY_CHECK_EQ(battle.events[index].sequence, index);
    }
    CY_CHECK_EQ(battle.session->event_count(), battle.events.size());
}

CY_TEST_CASE("combat: a projectile flies at its speed and deals its damage where it lands") {
    // 12 m/s at 60 ticks a second is 0.2 m a tick, truncated to 858 993 459 raw. Launched on tick 0
    // from 6 m off the target's centre, it moves once a tick from that same tick; on tick t it has
    // covered t steps, and it lands the first tick the rest is within one: 6 - 29 steps is 6 raw
    // more than a step, so tick 30.
    Battle battle;
    const u32 shooter = battle.unit(0, at(40, 80), archer(1, 20, 600, 12));
    const u32 target = battle.unit(1, at(100, 80), dummy(2, 100));
    battle.run(40);

    const std::vector<LockstepEvent> launches = battle.of(LockstepEventKind::Launch);
    CY_REQUIRE_EQ(launches.size(), 1U);
    CY_CHECK_EQ(launches[0].tick, 0U);
    CY_CHECK_EQ(launches[0].unit, shooter);
    CY_CHECK(launches[0].toward == at(100, 80));
    const std::vector<LockstepEvent> impacts = battle.of(LockstepEventKind::Impact);
    CY_REQUIRE_EQ(impacts.size(), 1U);
    CY_CHECK_EQ(impacts[0].tick, 30U);
    CY_CHECK_EQ(impacts[0].amount, 1U);
    CY_CHECK_EQ(impacts[0].projectile, launches[0].projectile);
    CY_CHECK(impacts[0].position == at(100, 80));
    const std::vector<LockstepEvent> damage = battle.of(LockstepEventKind::Damage);
    CY_REQUIRE_EQ(damage.size(), 1U);
    CY_CHECK_EQ(damage[0].tick, 30U);
    CY_CHECK_EQ(damage[0].amount, 20U);
    CY_CHECK_EQ(battle.health(target), 80U);
    CY_CHECK(battle.session->projectiles().empty());
}

CY_TEST_CASE("combat: a projectile homes on a target that moves, and lands where it now stands") {
    Battle battle;
    (void)battle.unit(0, at(40, 80), archer(1, 20, 600, 12));
    const u32 target = battle.unit(1, at(100, 80), dummy(2, 100));
    battle.order(LockstepOrderKind::Move, 1, at(160, 80));
    battle.run(60);
    const std::vector<LockstepEvent> impacts = battle.of(LockstepEventKind::Impact);
    CY_REQUIRE_EQ(impacts.size(), 1U);
    // Later than the standing target's tick 30, and on the target, not on where it was aimed.
    CY_CHECK_GT(impacts[0].tick, 30U);
    CY_CHECK(impacts[0].position.x > tenths(100));
    CY_CHECK_EQ(battle.health(target), 80U);
}

CY_TEST_CASE("combat: a splash hits every enemy its radius touches, and never a friend") {
    // A mortar's 2 m splash lands on B1. B2 stands 1.5 m on, its footprint within the radius; B3
    // 5 m on, outside it; the mortar's own friend 1.5 m to the side, within it, is untouched.
    Battle battle;
    const u32 mortar = battle.unit(0, at(40, 100), archer(1, 15, 600, 30, Fixed::from_int(2)));
    const u32 b1 = battle.unit(1, at(120, 100), dummy(2, 50));
    const u32 b2 = battle.unit(1, at(135, 100), dummy(2, 50));
    const u32 b3 = battle.unit(1, at(170, 100), dummy(2, 50));
    const u32 friend_unit = battle.unit(2, at(120, 115), dummy(1, 50));
    battle.run(60);
    const std::vector<LockstepEvent> impacts = battle.of(LockstepEventKind::Impact);
    CY_REQUIRE_EQ(impacts.size(), 1U);
    CY_CHECK_EQ(impacts[0].unit, mortar);
    CY_CHECK_EQ(impacts[0].amount, 2U);
    CY_CHECK_EQ(battle.health(b1), 35U);
    CY_CHECK_EQ(battle.health(b2), 35U);
    CY_CHECK_EQ(battle.health(b3), 50U);
    CY_CHECK_EQ(battle.health(friend_unit), 50U);
}

CY_TEST_CASE("combat: a structure takes hits through its armour, falls, and is no target after") {
    // 12 through 5 armour is 7 a blow, every 5 ticks: 30 -> 23 -> 16 -> 9 -> 2, and the fifth blow
    // takes the last 2 on tick 20. The unit stands 3 m off the centre of a 2 m footprint: a 0.5 m
    // gap, within its reach.
    Battle battle;
    LockstepStructureSpec tower;
    tower.team = 2;
    tower.position = at(140, 80);
    tower.radius = Fixed::from_int(2);
    tower.health = 30;
    tower.armour = 5;
    auto placed = battle.session->fortify(tower);
    CY_REQUIRE(placed.has_value());
    const u32 sapper = battle.unit(0, at(110, 80), melee(1, 50, 0, 12, 5));
    battle.run(40);
    const std::vector<LockstepEvent> damage = battle.of(LockstepEventKind::Damage);
    CY_REQUIRE_EQ(damage.size(), 5U);
    const u32 left[] = {23, 16, 9, 2, 0};
    for (usize index = 0; index < damage.size(); ++index) {
        CY_CHECK_EQ(damage[index].target.kind, LockstepTargetKind::Structure);
        CY_CHECK_EQ(damage[index].health, left[index]);
        CY_CHECK_EQ(damage[index].tick, static_cast<u64>(index * 5U));
    }
    const std::vector<LockstepEvent> fallen = battle.of(LockstepEventKind::Destroyed);
    CY_REQUIRE_EQ(fallen.size(), 1U);
    CY_CHECK_EQ(fallen[0].tick, 20U);
    CY_CHECK_EQ(fallen[0].team, 2U);
    CY_CHECK_EQ(fallen[0].unit, sapper);
    auto state = battle.session->structure(*placed);
    CY_REQUIRE(state.has_value());
    CY_CHECK(!state->standing);
    auto fighter = battle.session->combatant(sapper);
    CY_REQUIRE(fighter.has_value());
    CY_CHECK_EQ(fighter->kills, 1U);
    CY_CHECK(!fighter->target.some());
}

CY_TEST_CASE("combat: an attack-move fights what it meets, then goes on to its point") {
    Battle battle;
    const u32 lead = battle.unit(0, at(40, 60), melee(1, 80, 0, 15, 20));
    (void)battle.unit(0, at(40, 90), melee(1, 80, 0, 15, 20));
    const u32 picket = battle.unit(1, at(200, 70), dummy(2, 20));
    battle.order(LockstepOrderKind::AttackMove, 0, at(360, 80));
    battle.run(900);
    CY_CHECK(!battle.alive(picket));
    CY_CHECK_EQ(battle.of(LockstepEventKind::Death).size(), 1U);
    for (const u32 unit : {lead, lead + 1U}) {
        auto state = battle.session->unit(unit);
        auto fighter = battle.session->combatant(unit);
        CY_REQUIRE(state.has_value());
        CY_REQUIRE(fighter.has_value());
        // On, past the picket, to the point's formation; standing there, idle.
        CY_CHECK_LT(detmath::distance(state->position, at(360, 80)), Fixed::from_int(4));
        CY_CHECK_EQ(fighter->stance, game_backend::LockstepStance::Idle);
    }
}

CY_TEST_CASE("combat: an attack names the enemy nearest its point; a plain move ignores enemies") {
    Battle named;
    (void)named.unit(0, at(40, 60), archer(1, 30, 30, 20));
    const u32 near_enemy = named.unit(1, at(200, 60), dummy(2, 60));
    const u32 far_enemy = named.unit(1, at(200, 130), dummy(2, 60));
    // The point is beside the far one: it is the one attacked, though the other is nearer.
    named.order(LockstepOrderKind::Attack, 0, at(205, 130));
    named.run(400);
    CY_CHECK(!named.alive(far_enemy));
    const std::vector<LockstepEvent> damage = named.of(LockstepEventKind::Damage);
    CY_REQUIRE(!damage.empty());
    CY_CHECK_EQ(damage.front().target.index, far_enemy);
    CY_CHECK_EQ(damage.front().target.kind, LockstepTargetKind::Unit);
    (void)near_enemy;

    Battle passing;
    const u32 runner = passing.unit(0, at(40, 60), melee(1, 80, 0, 15, 20));
    (void)passing.unit(1, at(150, 60), dummy(2, 60));
    passing.order(LockstepOrderKind::Move, 0, at(300, 60));
    passing.run(300);
    // It walked by within reach and struck nothing; only once it stood idle could it look.
    for (const LockstepEvent& event : passing.of(LockstepEventKind::Strike)) {
        CY_CHECK_NE(event.unit, runner);
    }
}

CY_TEST_CASE("combat: what arming and fortifying refuse") {
    Battle battle;
    LockstepUnitSpec spec;
    spec.position = at(40, 60);
    auto unit = battle.session->enlist(spec);
    CY_REQUIRE(unit.has_value());
    LockstepCombatSpec broken = melee(1, 0, 0, 10, 10);
    CY_CHECK(!battle.session->arm(*unit, broken).has_value());
    broken = archer(1, 10, 10, 0);
    CY_CHECK(!battle.session->arm(*unit, broken).has_value());
    broken = melee(1, 10, 0, 10, 10);
    broken.range = Fixed::from_raw(-1);
    CY_CHECK(!battle.session->arm(*unit, broken).has_value());
    CY_CHECK(!battle.session->arm(*unit + 1U, melee(1, 10, 0, 10, 10)).has_value());
    CY_CHECK(!battle.session->combat());
    LockstepStructureSpec flat;
    flat.radius = Fixed::zero();
    CY_CHECK(!battle.session->fortify(flat).has_value());
    CY_CHECK(!battle.session->combat());
    CY_REQUIRE(battle.session->arm(*unit, melee(1, 10, 0, 10, 10)).has_value());
    CY_CHECK(battle.session->combat());
    battle.run(1);
    const auto late = battle.session->arm(*unit, melee(1, 10, 0, 10, 10));
    CY_REQUIRE(!late.has_value());
    CY_CHECK_EQ(late.error().code, ErrorCode::PermissionDenied);
    CY_CHECK(!battle.session->fortify(LockstepStructureSpec{}).has_value());
}

// --- Through the ABI, with two peers
// ---------------------------------------------------------------

namespace {

/// The issuer the adapter answers from and a follower driven by its log, as a host runs them.
struct Peers {
    navigation::NavMesh baked{allocator(), Name::intern("lockstep.combat.peers"), 64.0F};
    std::unique_ptr<LockstepSession> issuer;
    std::unique_ptr<LockstepSession> follower;
    std::unique_ptr<LockstepAdapter> adapter;
    abi::Host host{system_allocator(MemoryDomain::Scripting)};

    explicit Peers(u32 history = LockstepAdapter::kEventHistory) noexcept {
        CY_REQUIRE(movement_test::bake_rts_mesh(kMapCells, baked));
        issuer = std::make_unique<LockstepSession>(allocator(), LockstepConfig{});
        follower = std::make_unique<LockstepSession>(allocator(), LockstepConfig{});
        CY_REQUIRE(issuer->load(baked).has_value());
        CY_REQUIRE(follower->load(baked).has_value());
        adapter = std::make_unique<LockstepAdapter>(*issuer, follower.get(), history);
        game_backend::bind_lockstep(host, adapter.get());
    }

    [[nodiscard]] CyEngine engine() noexcept { return &host; }

    u32 soldier(u32 group, u32 team, i32 x, i32 z, bool ranged) noexcept {
        CyLockstepUnitDesc desc{};
        desc.struct_size = sizeof(desc);
        desc.group = group;
        desc.position = CyFixedVec2{static_cast<i64>(x) * kOne, static_cast<i64>(z) * kOne};
        u32 unit = 0;
        CY_REQUIRE_EQ(table().lockstep_enlist(engine(), &desc, &unit), CY_RESULT_OK);
        CyLockstepCombatDesc arm{};
        arm.struct_size = sizeof(arm);
        arm.team = team;
        arm.health = ranged ? 40 : 90;
        arm.armour = ranged ? 0 : 2;
        arm.weapon = ranged ? CY_LOCKSTEP_WEAPON_RANGED : CY_LOCKSTEP_WEAPON_MELEE;
        arm.damage = ranged ? 9 : 14;
        arm.cooldown = ranged ? 45 : 30;
        arm.range = ranged ? 9 * kOne : (kOne * 5) / 4;
        arm.sight = 12 * kOne;
        arm.projectile_speed = ranged ? 18 * kOne : 0;
        CY_REQUIRE_EQ(table().lockstep_arm(engine(), unit, &arm), CY_RESULT_OK);
        return unit;
    }

    void attack_move(u32 group, i32 x, i32 z) noexcept {
        CyLockstepOrder order{};
        order.struct_size = sizeof(order);
        order.kind = CY_LOCKSTEP_ORDER_ATTACK_MOVE;
        order.group = group;
        order.target = CyFixedVec2{static_cast<i64>(x) * kOne, static_cast<i64>(z) * kOne};
        const abi::game::PhaseScope fixed(host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_REQUIRE_EQ(table().lockstep_order(engine(), &order), CY_RESULT_OK);
    }

    /// Two armies of four and four meet, attacking-moving at each other from tick 0 when `ticks`
    /// runs any; returns the events read through the ABI, a cursor kept.
    std::vector<CyLockstepEvent> fight(u32 ticks) noexcept {
        for (u32 index = 0; index < 4; ++index) {
            (void)soldier(0, 1, 4 + static_cast<i32>(index), 6 + static_cast<i32>(index % 2) * 3,
                          index % 2 == 0);
            (void)soldier(1, 2, 40 + static_cast<i32>(index), 8 + static_cast<i32>(index % 2) * 3,
                          index % 2 == 1);
        }
        CyLockstepStructureDesc keep{};
        keep.struct_size = sizeof(keep);
        keep.team = 2;
        keep.position = CyFixedVec2{50 * kOne, 9 * kOne};
        keep.radius = 2 * kOne;
        keep.health = 120;
        u32 structure = 0;
        CY_REQUIRE_EQ(table().lockstep_fortify(engine(), &keep, &structure), CY_RESULT_OK);
        std::vector<CyLockstepEvent> read;
        u64 cursor = 0;
        for (u32 tick = 0; tick < ticks; ++tick) {
            if (tick == 0) {
                attack_move(0, 50, 9);
                attack_move(1, 6, 8);
            }
            CY_REQUIRE(adapter->tick().has_value());
            u32 count = 0;
            CY_REQUIRE_EQ(table().lockstep_events(engine(), cursor, nullptr, 0, &count),
                          CY_RESULT_OK);
            std::vector<CyLockstepEvent> batch(count);
            u32 written = 0;
            CY_REQUIRE_EQ(table().lockstep_events(engine(), cursor, batch.data(), count, &written),
                          CY_RESULT_OK);
            CY_REQUIRE_EQ(written, count);
            for (const CyLockstepEvent& event : batch) {
                read.push_back(event);
                cursor = event.sequence + 1U;
            }
        }
        return read;
    }
};

}  // namespace

CY_TEST_CASE("combat: two peers fight the same battle, tick for tick, through the ABI") {
    Peers peers;
    const std::vector<CyLockstepEvent> read = peers.fight(900);
    CY_CHECK_EQ(peers.adapter->disagreements(), 0U);
    CY_CHECK_EQ(peers.issuer->digest(), peers.follower->digest());
    CY_CHECK_EQ(peers.follower->event_count(), peers.issuer->event_count());
    // Every event read once, numbered without a gap, and a real battle: blows, flights, deaths.
    CY_REQUIRE_EQ(read.size(), peers.issuer->event_count());
    u32 kinds[6] = {};
    for (usize index = 0; index < read.size(); ++index) {
        CY_CHECK_EQ(read[index].sequence, index);
        CY_REQUIRE(read[index].kind < 6U);
        ++kinds[read[index].kind];
    }
    CY_CHECK_GT(kinds[CY_LOCKSTEP_EVENT_STRIKE], 0U);
    CY_CHECK_GT(kinds[CY_LOCKSTEP_EVENT_LAUNCH], 0U);
    CY_CHECK_EQ(
        kinds[CY_LOCKSTEP_EVENT_IMPACT] + 0U,
        kinds[CY_LOCKSTEP_EVENT_LAUNCH] - static_cast<u32>(peers.issuer->projectiles().size()));
    CY_CHECK_GT(kinds[CY_LOCKSTEP_EVENT_DEATH], 0U);
    // The ABI's view of a unit matches the session's.
    CyLockstepCombatant fighter{};
    fighter.struct_size = sizeof(fighter);
    CY_REQUIRE_EQ(table().lockstep_combatant(peers.engine(), 0, &fighter), CY_RESULT_OK);
    auto state = peers.issuer->combatant(0);
    CY_REQUIRE(state.has_value());
    CY_CHECK_EQ(fighter.health, state->health);
    CY_CHECK_EQ((fighter.flags & CY_LOCKSTEP_COMBATANT_ALIVE) != 0U, state->alive);
    CyLockstepStructure keep{};
    keep.struct_size = sizeof(keep);
    CY_REQUIRE_EQ(table().lockstep_structure(peers.engine(), 0, &keep), CY_RESULT_OK);
    CY_CHECK_EQ(keep.max_health, 120U);
    // Arming after the first tick is refused, as enlisting is.
    CyLockstepCombatDesc late{};
    late.struct_size = sizeof(late);
    late.health = 1;
    late.damage = 1;
    late.cooldown = 1;
    CY_CHECK_EQ(table().lockstep_arm(peers.engine(), 0, &late), CY_RESULT_PERMISSION_DENIED);
    abi::clear_last_error();
}

CY_TEST_CASE("combat: a follower armed differently disagrees from the first blow") {
    // The same positions, the same orders — none at all — and one point more damage on one side:
    // nothing moves, so only the combat state can tell the peers apart, and it must.
    Battle issuer;
    Battle follower;
    (void)issuer.unit(0, at(100, 60), melee(1, 50, 0, 12, 10));
    (void)issuer.unit(1, at(121, 60), melee(2, 50, 0, 7, 10));
    (void)follower.unit(0, at(100, 60), melee(1, 50, 0, 12, 10));
    (void)follower.unit(1, at(121, 60), melee(2, 50, 0, 8, 10));
    CY_REQUIRE(issuer.session->start().has_value());
    CY_REQUIRE(follower.session->start().has_value());
    CY_CHECK_NE(issuer.session->world_hash(), follower.session->world_hash());
    issuer.run(1);
    follower.run(1);
    CY_CHECK_NE(issuer.session->state_hash(), follower.session->state_hash());
    // And a pair that agrees about the specs agrees about the battle.
    Battle twin;
    (void)twin.unit(0, at(100, 60), melee(1, 50, 0, 12, 10));
    (void)twin.unit(1, at(121, 60), melee(2, 50, 0, 7, 10));
    twin.run(1);
    CY_CHECK_EQ(issuer.session->state_hash(), twin.session->state_hash());
    CY_CHECK_EQ(issuer.session->digest(), twin.session->digest());
}

CY_TEST_CASE("combat: the event history keeps the latest, numbered, when a reader falls behind") {
    // A history of eight, and a reader that reads only once the battle is over: it is handed the
    // last eight events the session made, oldest first, and can tell from their numbers how many
    // it missed.
    Peers peers(8);
    (void)peers.fight(0);
    for (u32 tick = 0; tick < 600; ++tick) {
        if (tick == 0) {
            peers.attack_move(0, 50, 9);
            peers.attack_move(1, 6, 8);
        }
        CY_REQUIRE(peers.adapter->tick().has_value());
    }
    const u64 made = peers.issuer->event_count();
    CY_REQUIRE(made > 8U);
    CY_CHECK_EQ(peers.adapter->history(), 8U);
    u32 count = 0;
    CY_REQUIRE_EQ(table().lockstep_events(peers.engine(), 0, nullptr, 0, &count), CY_RESULT_OK);
    CY_CHECK_EQ(count, 8U);
    CyLockstepEvent held[8] = {};
    CY_REQUIRE_EQ(table().lockstep_events(peers.engine(), 0, held, 8, &count), CY_RESULT_OK);
    CY_REQUIRE_EQ(count, 8U);
    for (u32 index = 0; index < 8; ++index) {
        CY_CHECK_EQ(held[index].sequence, made - 8U + index);
    }
    // From a cursor within the history, only what follows it; and a small buffer takes the oldest.
    CY_REQUIRE_EQ(table().lockstep_events(peers.engine(), made - 3U, held, 8, &count),
                  CY_RESULT_OK);
    CY_CHECK_EQ(count, 3U);
    CY_CHECK_EQ(held[0].sequence, made - 3U);
    CY_REQUIRE_EQ(table().lockstep_events(peers.engine(), 0, held, 2, &count), CY_RESULT_OK);
    CY_CHECK_EQ(count, 2U);
    CY_CHECK_EQ(held[0].sequence, made - 8U);
    abi::clear_last_error();
}
