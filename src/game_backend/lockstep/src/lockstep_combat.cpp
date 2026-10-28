// SPDX-License-Identifier: MIT
// Combat in the lockstep session. See cy/game_backend/lockstep_combat.h. Integer arithmetic only:
// health and damage in points, distances in `Fixed`, time in ticks.

#include <cy/core/memory/hash.h>
#include <cy/game_backend/lockstep_session.h>
#include <cy/movement/path.h>

#include <algorithm>

namespace cy::game_backend {
namespace {

[[nodiscard]] Error refused(ErrorCode code, const char* message) noexcept {
    return Error{code, message};
}

/// What a hit deals through `armour`: at least one point.
[[nodiscard]] u32 dealt(u32 damage, u32 armour) noexcept {
    return damage > armour ? damage - armour : 1U;
}

[[nodiscard]] u64 fold_vec(u64 fold, FixedVec2 v) noexcept {
    return hash_combine(hash_combine(fold, static_cast<u64>(v.x.raw)), static_cast<u64>(v.y.raw));
}

[[nodiscard]] u64 fold_target(u64 fold, LockstepTarget target) noexcept {
    return hash_combine(hash_combine(fold, static_cast<u32>(target.kind)), target.index);
}

}  // namespace

// --- Configuration
// ---------------------------------------------------------------------------------

Status LockstepSession::arm(u32 index, const LockstepCombatSpec& spec) noexcept {
    if (started_) {
        return make_unexpected(
            refused(ErrorCode::PermissionDenied, "a unit is armed before the first tick"));
    }
    if (index >= mover_.size()) {
        return make_unexpected(refused(ErrorCode::NotFound, "no unit was enlisted at that index"));
    }
    const bool ranged = spec.weapon == LockstepWeapon::Ranged;
    if (spec.health == 0 || spec.damage == 0 || spec.cooldown == 0 || spec.range.raw < 0 ||
        spec.sight.raw < 0 || spec.splash.raw < 0 || (ranged && spec.projectile_speed.raw <= 0) ||
        static_cast<u32>(spec.weapon) > static_cast<u32>(LockstepWeapon::Ranged)) {
        return make_unexpected(refused(ErrorCode::InvalidArgument,
                                       "a combat spec has health, damage, a cooldown, a reach "
                                       "that is not negative and, ranged, a projectile speed"));
    }
    Combatant& fighter = combatants_[index];
    fighter.spec = spec;
    fighter.state.armed = true;
    fighter.state.alive = true;
    fighter.state.team = spec.team;
    fighter.state.health = spec.health;
    fighter.state.max_health = spec.health;
    fighter.state.armour = spec.armour;
    combat_ = true;
    return ok();
}

Expected<u32, Error> LockstepSession::fortify(const LockstepStructureSpec& spec) noexcept {
    if (started_) {
        return make_unexpected(
            refused(ErrorCode::PermissionDenied, "a structure is placed before the first tick"));
    }
    if (spec.health == 0 || spec.radius.raw <= 0) {
        return make_unexpected(
            refused(ErrorCode::InvalidArgument, "a structure has health and a footprint"));
    }
    if (Status blocked = mover_.add_obstacle(detmath::FixedCircle{spec.position, spec.radius});
        !blocked) {
        return make_unexpected(blocked.error());
    }
    if (!structures_.push_back(Structure{spec, spec.health})) {
        return make_unexpected(refused(ErrorCode::OutOfMemory, "fortifying a structure"));
    }
    combat_ = true;
    return static_cast<u32>(structures_.size() - 1U);
}

// --- Reading -----------------------------------------------------------------------------------

Expected<LockstepCombatantState, Error> LockstepSession::combatant(u32 index) const noexcept {
    if (index >= mover_.size()) {
        return make_unexpected(refused(ErrorCode::NotFound, "no unit was enlisted at that index"));
    }
    return combatants_[index].state;
}

Expected<LockstepStructureState, Error> LockstepSession::structure(u32 index) const noexcept {
    if (index >= structures_.size()) {
        return make_unexpected(
            refused(ErrorCode::NotFound, "no structure was fortified at that index"));
    }
    const Structure& building = structures_[index];
    LockstepStructureState state;
    state.team = building.spec.team;
    state.entity = building.spec.entity;
    state.position = building.spec.position;
    state.radius = building.spec.radius;
    state.health = building.health;
    state.max_health = building.spec.health;
    state.standing = building.health > 0;
    return state;
}

// --- Targets -----------------------------------------------------------------------------------

bool LockstepSession::standing(LockstepTarget target) const noexcept {
    switch (target.kind) {
        case LockstepTargetKind::Unit:
            return target.index < combatants_.size() && combatants_[target.index].state.armed &&
                   combatants_[target.index].state.alive;
        case LockstepTargetKind::Structure:
            return target.index < structures_.size() && structures_[target.index].health > 0;
        case LockstepTargetKind::None:
            break;
    }
    return false;
}

FixedVec2 LockstepSession::where(LockstepTarget target) const noexcept {
    return target.kind == LockstepTargetKind::Structure ? structures_[target.index].spec.position
                                                        : mover_.position(target.index);
}

Fixed LockstepSession::footprint(LockstepTarget target) const noexcept {
    return target.kind == LockstepTargetKind::Structure ? structures_[target.index].spec.radius
                                                        : mover_.radius(target.index);
}

Fixed LockstepSession::gap(u32 unit, LockstepTarget target) const noexcept {
    return detmath::distance(mover_.position(unit), where(target)) - mover_.radius(unit) -
           footprint(target);
}

LockstepTarget LockstepSession::nearest_enemy(u32 team, FixedVec2 point,
                                              Fixed within) const noexcept {
    // Units first, then structures, each in index order; a later candidate wins only when strictly
    // nearer, so a tie goes to the unit, and to the lower index.
    LockstepTarget best;
    Fixed best_gap = within;
    const auto consider = [&](LockstepTarget candidate, u32 candidate_team) noexcept {
        if (candidate_team == team || !standing(candidate)) {
            return;
        }
        const Fixed edge = detmath::distance(point, where(candidate)) - footprint(candidate);
        if (edge < best_gap || (!best.some() && edge == best_gap)) {
            best = candidate;
            best_gap = edge;
        }
    };
    for (u32 index = 0; index < combatants_.size(); ++index) {
        consider(LockstepTarget{LockstepTargetKind::Unit, index}, combatants_[index].state.team);
    }
    for (u32 index = 0; index < structures_.size(); ++index) {
        consider(LockstepTarget{LockstepTargetKind::Structure, index},
                 structures_[index].spec.team);
    }
    return best;
}

LockstepTarget LockstepSession::acquire(u32 unit) const noexcept {
    const Combatant& fighter = combatants_[unit];
    // The sight is a gap like the range, so the unit's own radius widens the search.
    return nearest_enemy(fighter.state.team, mover_.position(unit),
                         fighter.spec.sight + mover_.radius(unit));
}

// --- Orders ------------------------------------------------------------------------------------

Status LockstepSession::order_attack(u32 group, FixedVec2 point, bool pick) noexcept {
    if (Status moved = move_group(group, point); !moved) {
        return moved;
    }
    for (u32 unit = 0; unit < mover_.size(); ++unit) {
        Combatant& fighter = combatants_[unit];
        if (groups_[unit] != group || !fighter.state.alive) {
            continue;
        }
        fighter.destination = paths_[unit].empty() ? mover_.position(unit) : paths_[unit].back();
        fighter.state.stance = LockstepStance::AttackMove;
        if (pick && fighter.state.armed) {
            const LockstepTarget named =
                nearest_enemy(fighter.state.team, point, kAttackPickRadius);
            if (named.some()) {
                fighter.state.stance = LockstepStance::Attack;
                fighter.state.target = named;
            }
        }
    }
    return ok();
}

void LockstepSession::arrive(u32 unit) noexcept {
    Combatant& fighter = combatants_[unit];
    if (fighter.state.chasing) {
        // The end of a chase's path, not of the order's: the next think plans again or fights.
        fighter.state.chasing = false;
        return;
    }
    if (fighter.state.stance == LockstepStance::Move ||
        fighter.state.stance == LockstepStance::AttackMove) {
        fighter.state.stance = LockstepStance::Idle;
    }
}

Status LockstepSession::plan(u32 unit, FixedVec2 to) noexcept {
    const movement::FixedPathResult result = movement::find_path(
        search_, mesh_, mover_.position(unit), to, config_.node_budget, corridor_, paths_[unit]);
    ++paths_planned_;
    paths_found_ += result.found ? 1U : 0U;
    cursors_[unit] = 0;
    moving_[unit] = paths_[unit].empty() ? 0U : 1U;
    return ok();
}

void LockstepSession::halt(u32 unit) noexcept {
    paths_[unit].clear();
    cursors_[unit] = 0;
    moving_[unit] = 0;
}

// --- One tick: think ---------------------------------------------------------------------------

Status LockstepSession::think() noexcept {
    for (u32 unit = 0; unit < mover_.size(); ++unit) {
        const LockstepCombatantState& state = combatants_[unit].state;
        if (!state.armed || !state.alive) {
            continue;
        }
        if (Status thought = think_unit(unit); !thought) {
            return thought;
        }
    }
    return ok();
}

Status LockstepSession::think_unit(u32 unit) noexcept {
    Combatant& fighter = combatants_[unit];
    LockstepCombatantState& state = fighter.state;
    state.cooldown = state.cooldown > 0 ? state.cooldown - 1U : 0U;
    state.engaged = false;
    if (!standing(state.target)) {
        const bool lost_named = state.stance == LockstepStance::Attack && state.target.some();
        state.target = LockstepTarget{};
        if (lost_named) {
            state.stance = LockstepStance::Idle;
        }
    }
    // A unit on a plain move ignores enemies; every other stance looks for one.
    if (!state.target.some() && state.stance != LockstepStance::Move) {
        state.target = acquire(unit);
    }
    if (!state.target.some()) {
        if (fighter.diverted) {
            // Whatever it stopped for or chased fell or got away: an attack-move takes up its way
            // again, anything else stands where it is.
            fighter.diverted = false;
            state.chasing = false;
            if (state.stance == LockstepStance::AttackMove) {
                return plan(unit, fighter.destination);
            }
            halt(unit);
        }
        return ok();
    }
    fighter.diverted = true;
    if (gap(unit, state.target) <= fighter.spec.range) {
        halt(unit);
        state.chasing = false;
        state.engaged = true;
        if (state.cooldown == 0) {
            state.cooldown = fighter.spec.cooldown;
            return fire(unit);
        }
        return ok();
    }
    if (!state.chasing || tick_ >= fighter.repath_at) {
        state.chasing = true;
        fighter.repath_at = tick_ + kChaseRepathTicks;
        return plan(unit, where(state.target));
    }
    return ok();
}

Status LockstepSession::fire(u32 unit) noexcept {
    const Combatant& fighter = combatants_[unit];
    const LockstepTarget target = fighter.state.target;
    const FixedVec2 from = mover_.position(unit);
    const FixedVec2 to = where(target);
    if (fighter.spec.weapon == LockstepWeapon::Melee) {
        LockstepEvent strike;
        strike.kind = LockstepEventKind::Strike;
        strike.unit = unit;
        strike.target = target;
        strike.team = fighter.state.team;
        strike.position = from;
        strike.toward = to;
        if (Status emitted = emit(strike); !emitted) {
            return emitted;
        }
        return deal(unit, fighter.state.team, target, fighter.spec.damage);
    }
    LockstepProjectile shot;
    shot.id = next_projectile_++;
    shot.shooter = unit;
    shot.team = fighter.state.team;
    shot.target = target;
    shot.damage = fighter.spec.damage;
    shot.launched = tick_;
    shot.origin = from;
    shot.position = from;
    shot.aim = to;
    shot.step = Fixed::from_raw(fighter.spec.projectile_speed.raw / mover_.params().tick_rate);
    shot.splash = fighter.spec.splash;
    if (!projectiles_.push_back(shot)) {
        return make_unexpected(refused(ErrorCode::OutOfMemory, "launching a projectile"));
    }
    LockstepEvent launch;
    launch.kind = LockstepEventKind::Launch;
    launch.unit = unit;
    launch.target = target;
    launch.projectile = shot.id;
    launch.team = shot.team;
    launch.position = from;
    launch.toward = to;
    return emit(launch);
}

// --- One tick: fly -----------------------------------------------------------------------------

Status LockstepSession::fly() noexcept {
    u32 kept = 0;
    // Survivors are compacted in place: `kept` never passes the shot being read.
    for (LockstepProjectile shot : projectiles_) {
        if (standing(shot.target)) {
            shot.aim = where(shot.target);
        }
        const Fixed remaining = detmath::distance(shot.position, shot.aim);
        if (remaining <= shot.step) {
            shot.position = shot.aim;
            if (Status landed = land(shot); !landed) {
                return landed;
            }
            continue;
        }
        shot.position += detmath::normalize(shot.aim - shot.position) * shot.step;
        projectiles_[kept++] = shot;
    }
    while (projectiles_.size() > kept) {
        projectiles_.pop_back();
    }
    return ok();
}

Status LockstepSession::land(const LockstepProjectile& shot) noexcept {
    LockstepEvent impact;
    impact.kind = LockstepEventKind::Impact;
    impact.unit = shot.shooter;
    impact.target = shot.target;
    impact.projectile = shot.id;
    impact.team = shot.team;
    impact.position = shot.position;
    impact.toward = shot.origin;
    if (shot.splash.raw == 0) {
        const bool hit = standing(shot.target);
        impact.amount = hit ? 1U : 0U;
        if (Status emitted = emit(impact); !emitted) {
            return emitted;
        }
        return hit ? deal(shot.shooter, shot.team, shot.target, shot.damage) : ok();
    }
    // A splash: every standing enemy whose footprint the radius touches, units then structures,
    // in index order — collected before any damage, so a death cannot change who else is hit.
    Array<LockstepTarget> touched(allocator_);
    const auto touch = [&](LockstepTarget candidate, u32 team) noexcept {
        if (team == shot.team || !standing(candidate)) {
            return true;
        }
        const Fixed edge =
            detmath::distance(shot.position, where(candidate)) - footprint(candidate);
        return edge > shot.splash || touched.push_back(candidate).has_value();
    };
    for (u32 index = 0; index < combatants_.size(); ++index) {
        if (!touch(LockstepTarget{LockstepTargetKind::Unit, index},
                   combatants_[index].state.team)) {
            return make_unexpected(refused(ErrorCode::OutOfMemory, "a projectile's splash"));
        }
    }
    for (u32 index = 0; index < structures_.size(); ++index) {
        if (!touch(LockstepTarget{LockstepTargetKind::Structure, index},
                   structures_[index].spec.team)) {
            return make_unexpected(refused(ErrorCode::OutOfMemory, "a projectile's splash"));
        }
    }
    impact.amount = static_cast<u32>(touched.size());
    if (Status emitted = emit(impact); !emitted) {
        return emitted;
    }
    for (const LockstepTarget target : touched.span()) {
        if (Status dealt_to = deal(shot.shooter, shot.team, target, shot.damage); !dealt_to) {
            return dealt_to;
        }
    }
    return ok();
}

Status LockstepSession::deal(u32 attacker, u32 team, LockstepTarget target, u32 damage) noexcept {
    if (!standing(target)) {
        return ok();
    }
    const bool structure = target.kind == LockstepTargetKind::Structure;
    u32& health =
        structure ? structures_[target.index].health : combatants_[target.index].state.health;
    const u32 armour =
        structure ? structures_[target.index].spec.armour : combatants_[target.index].spec.armour;
    const u32 amount = std::min(dealt(damage, armour), health);
    health -= amount;
    LockstepEvent hurt;
    hurt.kind = LockstepEventKind::Damage;
    hurt.unit = attacker;
    hurt.target = target;
    hurt.amount = amount;
    hurt.health = health;
    hurt.team = team;
    hurt.position = where(target);
    if (Status emitted = emit(hurt); !emitted) {
        return emitted;
    }
    if (health > 0) {
        return ok();
    }
    if (attacker < combatants_.size()) {
        ++combatants_[attacker].state.kills;
    }
    LockstepEvent fell;
    fell.position = where(target);
    if (structure) {
        fell.kind = LockstepEventKind::Destroyed;
        fell.target = target;
        fell.unit = attacker;
        fell.team = structures_[target.index].spec.team;
        return emit(fell);
    }
    Combatant& victim = combatants_[target.index];
    victim.state.alive = false;
    victim.state.engaged = false;
    victim.state.chasing = false;
    victim.state.target = LockstepTarget{};
    halt(target.index);
    mover_.set_desired_velocity(target.index, FixedVec2{});
    mover_.deactivate(target.index);
    fell.kind = LockstepEventKind::Death;
    fell.unit = target.index;
    fell.target = target;
    fell.team = victim.state.team;
    return emit(fell);
}

Status LockstepSession::emit(LockstepEvent event) noexcept {
    event.sequence = next_event_++;
    event.tick = tick_;
    if (!events_.push_back(event)) {
        return make_unexpected(refused(ErrorCode::OutOfMemory, "recording a combat event"));
    }
    return ok();
}

// --- Hashes --------------------------------------------------------------------------------------

u64 LockstepSession::fold_combat_state(u64 fold) const noexcept {
    for (const Combatant& fighter : combatants_.span()) {
        const LockstepCombatantState& state = fighter.state;
        fold = hash_combine(fold, (state.armed ? 1U : 0U) | (state.alive ? 2U : 0U) |
                                      (state.engaged ? 4U : 0U) | (state.chasing ? 8U : 0U));
        fold = hash_combine(hash_combine(fold, state.health), state.cooldown);
        fold = hash_combine(hash_combine(fold, state.kills), static_cast<u32>(state.stance));
        fold = fold_target(fold, state.target);
        fold = hash_combine(fold_vec(fold, fighter.destination), fighter.repath_at);
        fold = hash_combine(fold, fighter.diverted ? 1U : 0U);
    }
    for (const Structure& building : structures_.span()) {
        fold = hash_combine(fold, building.health);
    }
    for (const LockstepProjectile& shot : projectiles_.span()) {
        fold = hash_combine(hash_combine(fold, shot.id), shot.shooter);
        fold = fold_target(fold, shot.target);
        fold = fold_vec(fold_vec(fold, shot.position), shot.aim);
    }
    return hash_combine(hash_combine(fold, next_event_), next_projectile_);
}

u64 LockstepSession::fold_combat_world(u64 fold) const noexcept {
    for (const Combatant& fighter : combatants_.span()) {
        const LockstepCombatSpec& spec = fighter.spec;
        fold = hash_combine(fold, fighter.state.armed ? 1U : 0U);
        if (!fighter.state.armed) {
            continue;
        }
        for (const u32 value : {spec.team, spec.health, spec.armour, static_cast<u32>(spec.weapon),
                                spec.damage, spec.cooldown}) {
            fold = hash_combine(fold, value);
        }
        for (const Fixed value : {spec.range, spec.sight, spec.projectile_speed, spec.splash}) {
            fold = hash_combine(fold, static_cast<u64>(value.raw));
        }
    }
    for (const Structure& building : structures_.span()) {
        const LockstepStructureSpec& spec = building.spec;
        fold = fold_vec(hash_combine(fold, spec.team), spec.position);
        fold = hash_combine(fold, static_cast<u64>(spec.radius.raw));
        fold = hash_combine(hash_combine(fold, spec.health), spec.armour);
    }
    return fold;
}

}  // namespace cy::game_backend
