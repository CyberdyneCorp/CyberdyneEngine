// SPDX-License-Identifier: MIT
#pragma once
// Combat in a lockstep session: health, weapons, projectiles, structures and the events they make.
// openspec/changes/add-rts-combat-and-minimap. What ABI 1.16's `lockstep_arm`, `lockstep_fortify`,
// `lockstep_combatant`, `lockstep_structure`, `lockstep_events` and `lockstep_projectiles` reach.
//
// EVERY QUANTITY IS AN INTEGER. Health and damage are whole points; ranges, sight, positions and a
// projectile's flight are `Fixed`; cooldowns are ticks. A peer that executes the same command log
// computes the same hits, the same deaths and the same flights on any architecture, and the
// session's state hash folds all of it — so a follower that disagrees about one point of damage
// disagrees about the tick it happened on.
//
// WHAT A TICK DOES, after the orders execute and before units move (lockstep_session.h):
//
//   think     every armed, living unit in index order: count its cooldown down; keep its target
//             while it lives, or acquire the nearest enemy within its sight (an attack-move, an
//             idle unit, an attack whose target died); when the gap to the target is within its
//             range, stand and strike — a melee blow lands now, a ranged weapon launches a
//             projectile — and otherwise chase it, planning a path every `kChaseRepathTicks`
//
// and after units moved:
//
//   fly       every projectile in launch order homes on its target's new position (or flies on to
//             where its target fell) by its speed; one that reaches its aim point lands: it damages
//             its target, or with a splash radius every enemy whose footprint the radius touches
//
// Damage is `max(1, damage - armour)`; health reaching zero kills a unit — it stops, leaves the
// crowd (`KinematicMover::deactivate`) and can no longer be targeted — or destroys a structure.
//
// THE FEATURE OFF IS THE SESSION BEFORE IT. A session in which nothing was armed and nothing
// fortified runs, hashes and digests exactly as it did before combat existed: the combat state is
// folded into the hashes only when there is some.

#include <cy/core/base/types.h>
#include <cy/core/detmath/fixed.h>
#include <cy/core/detmath/vec.h>

namespace cy::game_backend {

/// How a unit attacks: `CyLockstepWeapon`, value for value.
enum class LockstepWeapon : u32 { Melee = 0, Ranged = 1 };

/// What a unit is attacking: `CyLockstepTargetKind`, value for value.
enum class LockstepTargetKind : u32 { None = 0, Unit = 1, Structure = 2 };

struct LockstepTarget {
    LockstepTargetKind kind = LockstepTargetKind::None;
    u32 index = 0;

    [[nodiscard]] bool some() const noexcept { return kind != LockstepTargetKind::None; }
    friend constexpr bool operator==(const LockstepTarget&, const LockstepTarget&) = default;
};

/// A unit's fighting kit, as `arm` takes it. Every field is part of the world hash.
struct LockstepCombatSpec {
    /// Units and structures of another team are enemies.
    u32 team = 0;
    /// Starting and most health, > 0.
    u32 health = 100;
    /// Subtracted from every hit it takes; a hit always deals at least one point.
    u32 armour = 0;
    LockstepWeapon weapon = LockstepWeapon::Melee;
    /// Points one hit deals before armour, > 0.
    u32 damage = 10;
    /// Ticks between two attacks, > 0.
    u32 cooldown = 30;
    /// How far it reaches: the gap between its footprint and its target's.
    detmath::Fixed range = detmath::Fixed::half();
    /// How far it notices an enemy to attack, the same gap.
    detmath::Fixed sight = detmath::Fixed::from_int(8);
    /// A ranged weapon's projectile speed, metres a second, > 0 for a ranged weapon; a melee
    /// weapon's is zero, as the ABI's unset field is, so a spec reads the same however it came.
    detmath::Fixed projectile_speed;
    /// A ranged weapon's splash: zero hits the target alone; otherwise every enemy whose footprint
    /// is within this radius of where it lands.
    detmath::Fixed splash;
};

/// A building: a static target that blocks movement. `fortify` takes it.
struct LockstepStructureSpec {
    u32 team = 0;
    /// Presentation only: the scene entity that draws it. Not hashed.
    u64 entity = 0;
    detmath::FixedVec2 position;
    /// Its footprint, a circle; also the obstacle units walk round. > 0.
    detmath::Fixed radius = detmath::Fixed::from_int(2);
    u32 health = 500;
    u32 armour = 0;
};

/// What happened: `CyLockstepEventKind`, value for value.
enum class LockstepEventKind : u32 {
    /// A melee blow: `unit` struck `target`, from `position` towards `toward`.
    Strike = 0,
    /// A projectile left `unit` at `position`, aimed at `toward`.
    Launch = 1,
    /// A projectile landed at `position`; `amount` is the enemies its splash touched, or 0 or 1.
    Impact = 2,
    /// `target` took `amount` points from `unit`, leaving `health`; `position` is where it stands.
    Damage = 3,
    /// Unit `unit` of `team` died at `position`.
    Death = 4,
    /// Structure `target.index` of `team` was destroyed at `position`.
    Destroyed = 5,
};

/// "No unit": an event with no acting unit.
inline constexpr u32 kLockstepNoUnit = 0xFFFF'FFFFU;

struct LockstepEvent {
    /// From zero, in the order the session made them: a peer's events are its log's.
    u64 sequence = 0;
    u64 tick = 0;
    LockstepEventKind kind = LockstepEventKind::Strike;
    u32 unit = kLockstepNoUnit;
    LockstepTarget target;
    u32 amount = 0;
    u32 health = 0;
    u32 projectile = 0;
    u32 team = 0;
    detmath::FixedVec2 position;
    detmath::FixedVec2 toward;
};

/// A projectile in flight.
struct LockstepProjectile {
    u32 id = 0;
    u32 shooter = 0;
    u32 team = 0;
    LockstepTarget target;
    u32 damage = 0;
    u64 launched = 0;
    detmath::FixedVec2 origin;
    detmath::FixedVec2 position;
    detmath::FixedVec2 aim;
    /// Metres it covers a tick.
    detmath::Fixed step;
    detmath::Fixed splash;
};

/// What a unit is doing, as far as fighting goes: `CyLockstepStance`, value for value.
enum class LockstepStance : u32 {
    /// Standing: it fights what comes within its sight.
    Idle = 0,
    /// Going where it was sent, ignoring enemies until it arrives.
    Move = 1,
    /// Going where it was sent, fighting what it meets on the way.
    AttackMove = 2,
    /// Attacking the one target it was given, wherever it goes, until it falls.
    Attack = 3,
};

/// One unit's combat state, as the last tick left it.
struct LockstepCombatantState {
    bool armed = false;
    bool alive = true;
    /// Its target was within its range on the last tick.
    bool engaged = false;
    /// It is following a path to its target.
    bool chasing = false;
    u32 team = 0;
    u32 health = 0;
    u32 max_health = 0;
    u32 armour = 0;
    u32 cooldown = 0;
    u32 kills = 0;
    LockstepTarget target;
    LockstepStance stance = LockstepStance::Idle;
};

/// One structure's state.
struct LockstepStructureState {
    u32 team = 0;
    u64 entity = 0;
    detmath::FixedVec2 position;
    detmath::Fixed radius;
    u32 health = 0;
    u32 max_health = 0;
    bool standing = true;
};

/// Ticks between two paths a chasing unit plans to its moving target.
inline constexpr u32 kChaseRepathTicks = 15;
/// How near the order's point an enemy must stand for an Attack order to name it, beyond its
/// footprint. Further, the order is an attack-move to the point.
inline constexpr detmath::Fixed kAttackPickRadius = detmath::Fixed::from_int(3);

}  // namespace cy::game_backend
