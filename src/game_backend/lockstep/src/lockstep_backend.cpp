// SPDX-License-Identifier: MIT
// The lockstep adapter. See cy/game_backend/lockstep_backend.h.

#include <cy/abi/errors.h>
#include <cy/core/detmath/version.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/lockstep_backend.h>

namespace cy::game_backend {
namespace {

/// A session refusal, reported with its own sentence and mapped to the ABI's code.
[[nodiscard]] CyResult refuse(const Error& error) noexcept {
    return abi::report(error);
}

[[nodiscard]] CyLockstepEvent flatten(const LockstepEvent& event) noexcept {
    CyLockstepEvent out{};
    out.sequence = event.sequence;
    out.tick = event.tick;
    out.kind = static_cast<u32>(event.kind);
    out.unit = event.unit;
    out.target_kind = static_cast<u32>(event.target.kind);
    out.target = event.target.index;
    out.amount = event.amount;
    out.health = event.health;
    out.projectile = event.projectile;
    out.team = event.team;
    out.position = CyFixedVec2{event.position.x.raw, event.position.y.raw};
    out.toward = CyFixedVec2{event.toward.x.raw, event.toward.y.raw};
    return out;
}

[[nodiscard]] CyLockstepProjectile flatten(const LockstepProjectile& shot) noexcept {
    CyLockstepProjectile out{};
    out.id = shot.id;
    out.shooter = shot.shooter;
    out.team = shot.team;
    out.target_kind = static_cast<u32>(shot.target.kind);
    out.target = shot.target.index;
    out.launched = shot.launched;
    out.origin = CyFixedVec2{shot.origin.x.raw, shot.origin.y.raw};
    out.position = CyFixedVec2{shot.position.x.raw, shot.position.y.raw};
    out.aim = CyFixedVec2{shot.aim.x.raw, shot.aim.y.raw};
    return out;
}

[[nodiscard]] LockstepCombatSpec spec_of(const CyLockstepCombatDesc& desc) noexcept {
    LockstepCombatSpec spec;
    spec.team = desc.team;
    spec.health = desc.health;
    spec.armour = desc.armour;
    spec.weapon = static_cast<LockstepWeapon>(desc.weapon);
    spec.damage = desc.damage;
    spec.cooldown = desc.cooldown;
    spec.range = Fixed::from_raw(desc.range);
    spec.sight = Fixed::from_raw(desc.sight);
    spec.projectile_speed = Fixed::from_raw(desc.projectile_speed);
    spec.splash = Fixed::from_raw(desc.splash);
    return spec;
}

}  // namespace

LockstepAdapter::LockstepAdapter(LockstepSession& issuer, LockstepSession* follower,
                                 u32 history) noexcept
    : issuer_(issuer),
      follower_(follower),
      history_(system_allocator(MemoryDomain::World)),
      capacity_(history > 0U ? history : 1U) {}

CyResult LockstepAdapter::enlist(const CyLockstepUnitDesc& desc, u32& out_unit) noexcept {
    LockstepUnitSpec spec;
    spec.group = desc.group;
    spec.entity = desc.entity;
    spec.position = FixedVec2{Fixed::from_raw(desc.position.x), Fixed::from_raw(desc.position.y)};
    spec.radius = Fixed::from_raw(desc.radius);
    spec.max_speed = Fixed::from_raw(desc.max_speed);
    auto enlisted = issuer_.enlist(spec);
    if (!enlisted) {
        return refuse(enlisted.error());
    }
    if (follower_ != nullptr) {
        if (auto mirrored = follower_->enlist(spec); !mirrored) {
            return refuse(mirrored.error());
        }
    }
    out_unit = *enlisted;
    return CY_RESULT_OK;
}

CyResult LockstepAdapter::order(const CyLockstepOrder& order) noexcept {
    LockstepOrderPayload payload;
    payload.kind = order.kind;
    payload.group = order.group;
    payload.target_x = order.target.x;
    payload.target_z = order.target.y;
    if (Status recorded = issuer_.record(payload); !recorded) {
        return refuse(recorded.error());
    }
    return CY_RESULT_OK;
}

CyResult LockstepAdapter::unit(u32 index, CyLockstepUnit& out) const noexcept {
    auto state = issuer_.unit(index);
    if (!state) {
        return refuse(state.error());
    }
    out.group = state->group;
    out.entity = state->entity;
    out.position = CyFixedVec2{state->position.x.raw, state->position.y.raw};
    out.velocity = CyFixedVec2{state->velocity.x.raw, state->velocity.y.raw};
    out.height = state->height.raw;
    out.heading = state->heading.raw;
    out.flags = (state->moving ? CY_LOCKSTEP_UNIT_MOVING : 0U) |
                (state->just_arrived ? CY_LOCKSTEP_UNIT_ARRIVED : 0U);
    return CY_RESULT_OK;
}

CyResult LockstepAdapter::status(CyLockstepStatus& out) const noexcept {
    out.units = issuer_.units();
    out.tick = issuer_.tick();
    out.commands = issuer_.log().size();
    out.state_hash = issuer_.state_hash();
    out.digest = issuer_.digest();
    out.kernel_version = detmath::kKernelVersion;
    out.disagreements = disagreements_;
    return CY_RESULT_OK;
}

CyResult LockstepAdapter::arm(u32 unit, const CyLockstepCombatDesc& desc) noexcept {
    const LockstepCombatSpec spec = spec_of(desc);
    if (Status armed = issuer_.arm(unit, spec); !armed) {
        return refuse(armed.error());
    }
    if (follower_ != nullptr) {
        if (Status mirrored = follower_->arm(unit, spec); !mirrored) {
            return refuse(mirrored.error());
        }
    }
    return CY_RESULT_OK;
}

CyResult LockstepAdapter::fortify(const CyLockstepStructureDesc& desc,
                                  u32& out_structure) noexcept {
    LockstepStructureSpec spec;
    spec.team = desc.team;
    spec.entity = desc.entity;
    spec.position = FixedVec2{Fixed::from_raw(desc.position.x), Fixed::from_raw(desc.position.y)};
    spec.radius = Fixed::from_raw(desc.radius);
    spec.health = desc.health;
    spec.armour = desc.armour;
    auto placed = issuer_.fortify(spec);
    if (!placed) {
        return refuse(placed.error());
    }
    if (follower_ != nullptr) {
        if (auto mirrored = follower_->fortify(spec); !mirrored) {
            return refuse(mirrored.error());
        }
    }
    out_structure = *placed;
    return CY_RESULT_OK;
}

CyResult LockstepAdapter::combatant(u32 unit, CyLockstepCombatant& out) const noexcept {
    auto state = issuer_.combatant(unit);
    if (!state) {
        return refuse(state.error());
    }
    out.flags = (state->armed ? CY_LOCKSTEP_COMBATANT_ARMED : 0U) |
                (state->alive ? CY_LOCKSTEP_COMBATANT_ALIVE : 0U) |
                (state->engaged ? CY_LOCKSTEP_COMBATANT_ENGAGED : 0U) |
                (state->chasing ? CY_LOCKSTEP_COMBATANT_CHASING : 0U);
    out.team = state->team;
    out.health = state->health;
    out.max_health = state->max_health;
    out.armour = state->armour;
    out.cooldown = state->cooldown;
    out.kills = state->kills;
    out.stance = static_cast<u32>(state->stance);
    out.target_kind = static_cast<u32>(state->target.kind);
    out.target = state->target.index;
    return CY_RESULT_OK;
}

CyResult LockstepAdapter::structure(u32 index, CyLockstepStructure& out) const noexcept {
    auto state = issuer_.structure(index);
    if (!state) {
        return refuse(state.error());
    }
    out.team = state->team;
    out.entity = state->entity;
    out.position = CyFixedVec2{state->position.x.raw, state->position.y.raw};
    out.radius = state->radius.raw;
    out.health = state->health;
    out.max_health = state->max_health;
    return CY_RESULT_OK;
}

CyResult LockstepAdapter::events(u64 first, CyLockstepEvent* out, u32 capacity,
                                 u32& out_count) const noexcept {
    const u32 held = history_.size();
    u32 count = 0;
    for (u32 offset = 0; offset < held; ++offset) {
        const LockstepEvent& event = history_[(oldest_ + offset) % held];
        if (event.sequence < first) {
            continue;
        }
        if (out != nullptr) {
            if (count == capacity) {
                break;
            }
            out[count] = flatten(event);
        }
        ++count;
    }
    out_count = count;
    return CY_RESULT_OK;
}

CyResult LockstepAdapter::projectiles(CyLockstepProjectile* out, u32 capacity,
                                      u32& out_count) const noexcept {
    const Span<const LockstepProjectile> flying = issuer_.projectiles();
    if (out == nullptr) {
        out_count = static_cast<u32>(flying.size());
        return CY_RESULT_OK;
    }
    u32 count = 0;
    for (const LockstepProjectile& shot : flying) {
        if (count == capacity) {
            break;
        }
        out[count++] = flatten(shot);
    }
    out_count = count;
    return CY_RESULT_OK;
}

Status LockstepAdapter::keep(Span<const LockstepEvent> made) noexcept {
    for (const LockstepEvent& event : made) {
        if (history_.size() < capacity_) {
            if (Status kept = history_.push_back(event); !kept) {
                return kept;
            }
            continue;
        }
        history_[oldest_] = event;
        oldest_ = (oldest_ + 1U) % capacity_;
    }
    return ok();
}

Status LockstepAdapter::tick(jobs::JobSystem* jobs) noexcept {
    if (Status advanced = issuer_.advance(jobs); !advanced) {
        return advanced;
    }
    if (Status kept = keep(issuer_.events()); !kept) {
        return kept;
    }
    if (follower_ == nullptr) {
        return ok();
    }
    // The follower runs the tick the issuer just ran, from the issuer's log alone.
    if (Status received = follower_->receive(issuer_.log(), follower_cursor_); !received) {
        return received;
    }
    if (Status advanced = follower_->advance(jobs); !advanced) {
        return advanced;
    }
    disagreements_ += follower_->state_hash() == issuer_.state_hash() ? 0U : 1U;
    return ok();
}

void bind_lockstep(abi::Host& host, LockstepAdapter* adapter) noexcept {
    host.game.lockstep = adapter;
}

}  // namespace cy::game_backend
