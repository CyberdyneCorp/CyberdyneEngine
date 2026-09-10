// Ability definitions, sets, grants, per-owner state, costs and cooldowns. M8.b tasks 4.1 and 4.2.

#include <cy/gameplay/abilities/abilities.h>

#include <algorithm>
#include <utility>

namespace cy::gameplay::abilities {
namespace {

constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;

[[nodiscard]] u64 mix(u64 hash, u64 value) noexcept {
    for (u32 byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8U)) & 0xFFULL;
        hash *= kFnvPrime;
    }
    return hash;
}

}  // namespace

const char* prediction_policy_name(PredictionPolicy policy) noexcept {
    switch (policy) {
        case PredictionPolicy::None:
            return "None";
        case PredictionPolicy::ClientPredicted:
            return "ClientPredicted";
        case PredictionPolicy::AuthorityOnly:
            return "AuthorityOnly";
        case PredictionPolicy::DeterministicLockstep:
            return "DeterministicLockstep";
        case PredictionPolicy::Count:
            break;
    }
    return "AuthorityOnly";
}

CostLedger::CostLedger(Allocator& allocator, AttributeStore& attributes) noexcept
    : attributes_(&attributes), reservations_(allocator) {}

f32 CostLedger::available(ecs::Entity owner, AttributeId attribute) const noexcept {
    f32 value = attributes_->current(owner, attribute);
    for (const CostReservation& reservation : reservations_) {
        if (reservation.owner == owner && reservation.attribute == attribute) {
            value -= reservation.amount;
        }
    }
    return value;
}

Status CostLedger::reserve(ecs::Entity owner, AttributeId attribute, f32 amount,
                           u64 activation) noexcept {
    if (available(owner, attribute) < amount) {
        return make_unexpected(
            Error{ErrorCode::Unavailable, "the resource is already spoken for this tick", 0});
    }
    return reservations_.push_back(CostReservation{owner, attribute, amount, activation});
}

Status CostLedger::commit(u64 activation) noexcept {
    usize index = reservations_.size();
    while (index-- > 0) {
        const CostReservation& reservation = reservations_[index];
        if (reservation.activation != activation) {
            continue;
        }
        const f32 base = attributes_->base(reservation.owner, reservation.attribute);
        if (Status spent = attributes_->set_base(reservation.owner, reservation.attribute,
                                                 base - reservation.amount);
            !spent) {
            return spent;
        }
        reservations_.erase(index);
    }
    return ok();
}

void CostLedger::release(u64 activation) noexcept {
    usize index = reservations_.size();
    while (index-- > 0) {
        if (reservations_[index].activation == activation) {
            reservations_.erase(index);
        }
    }
}

AbilityRegistry::AbilityRegistry(Allocator& allocator) noexcept
    : allocator_(&allocator),
      definitions_(allocator),
      costs_(allocator),
      effects_(allocator),
      sets_(allocator),
      grants_(allocator),
      states_(allocator),
      index_(allocator) {}

Expected<AbilityId, Error> AbilityRegistry::declare(const AbilityDefinition& definition,
                                                    Span<const Cost> costs,
                                                    Span<const EffectDefId> effects) noexcept {
    if (definition.stable_id == 0) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "an ability's stable identity is never zero", 0});
    }
    if (find(definition.stable_id) != kInvalidAbility) {
        return make_unexpected(
            Error{ErrorCode::AlreadyExists, "that ability identity is declared", 0});
    }
    AbilityDefinition stored = definition;
    stored.first_cost = static_cast<u32>(costs_.size());
    stored.cost_count = static_cast<u32>(costs.size());
    if (Status appended = costs_.append(costs); !appended) {
        return make_unexpected(appended.error());
    }
    stored.first_effect = static_cast<u32>(effects_.size());
    stored.effect_count = static_cast<u32>(effects.size());
    if (Status appended = effects_.append(effects); !appended) {
        return make_unexpected(appended.error());
    }
    if (Status pushed = definitions_.push_back(stored); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<AbilityId>(definitions_.size() - 1);
}

AbilityId AbilityRegistry::find(u32 stable_id) const noexcept {
    for (usize index = 0; index < definitions_.size(); ++index) {
        if (definitions_[index].stable_id == stable_id) {
            return static_cast<AbilityId>(index);
        }
    }
    return kInvalidAbility;
}

Span<const Cost> AbilityRegistry::costs_of(AbilityId id) const noexcept {
    if (id >= definitions_.size()) {
        return {};
    }
    return {costs_.data() + definitions_[id].first_cost, definitions_[id].cost_count};
}

Span<const EffectDefId> AbilityRegistry::effects_of(AbilityId id) const noexcept {
    if (id >= definitions_.size()) {
        return {};
    }
    return {effects_.data() + definitions_[id].first_effect, definitions_[id].effect_count};
}

Expected<AbilitySetId, Error> AbilityRegistry::create_set(Name name) noexcept {
    AbilitySet set{static_cast<AbilitySetId>(sets_.size()), name, Array<AbilityId>(*allocator_)};
    const AbilitySetId id = set.id;
    if (Status pushed = sets_.push_back(std::move(set)); !pushed) {
        return make_unexpected(pushed.error());
    }
    return id;
}

Status AbilityRegistry::add_to_set(AbilitySetId set, AbilityId ability) noexcept {
    if (set >= sets_.size() || ability >= definitions_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such ability set or ability", 0});
    }
    return sets_[set].members.push_back(ability);
}

Span<const AbilityId> AbilityRegistry::set_members(AbilitySetId set) const noexcept {
    return set < sets_.size() ? sets_[set].members.span() : Span<const AbilityId>();
}

AbilityRegistry::OwnerRow* AbilityRegistry::row_of(ecs::Entity owner) noexcept {
    const u32* slot = index_.find(owner.bits());
    return slot != nullptr && *slot < states_.size() ? &states_[*slot] : nullptr;
}

const AbilityRegistry::OwnerRow* AbilityRegistry::row_of(ecs::Entity owner) const noexcept {
    const u32* slot = index_.find(owner.bits());
    return slot != nullptr && *slot < states_.size() ? &states_[*slot] : nullptr;
}

Expected<AbilityRegistry::OwnerRow*, Error> AbilityRegistry::ensure_row(
    ecs::Entity owner) noexcept {
    if (OwnerRow* found = row_of(owner); found != nullptr) {
        return found;
    }
    OwnerRow row{owner, Array<AbilityState>(*allocator_), Array<TagId>(*allocator_),
                 Array<i64>(*allocator_)};
    if (Status pushed = states_.push_back(std::move(row)); !pushed) {
        return make_unexpected(pushed.error());
    }
    auto placed = index_.insert(owner.bits(), static_cast<u32>(states_.size() - 1));
    if (!placed) {
        states_.pop_back();
        return make_unexpected(placed.error());
    }
    return &states_[states_.size() - 1];
}

Status AbilityRegistry::add_state(OwnerRow& row, AbilityId ability, GrantId grant,
                                  i64 tick) noexcept {
    for (AbilityState& state : row.abilities) {
        if (state.ability == ability) {
            // Already granted by another source. The state stays exactly as it is — a second grant
            // must not reset a running cooldown — and the grant record remembers both.
            return ok();
        }
    }
    AbilityState state;
    state.ability = ability;
    state.ready_tick = tick;
    state.charges = definitions_[ability].max_charges;
    state.next_recharge_tick = tick;
    state.grant = grant;
    return row.abilities.push_back(state);
}

Expected<GrantId, Error> AbilityRegistry::grant_set(ecs::Entity owner, AbilitySetId set,
                                                    Name source, i64 tick) noexcept {
    if (set >= sets_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such ability set", 0});
    }
    auto row = ensure_row(owner);
    if (!row) {
        return make_unexpected(row.error());
    }
    GrantRecord record;
    record.id = next_grant_++;
    record.owner = owner;
    record.set = set;
    record.source = source;
    record.active = true;
    if (Status pushed = grants_.push_back(record); !pushed) {
        return make_unexpected(pushed.error());
    }
    for (const AbilityId ability : sets_[set].members) {
        if (Status added = add_state(*row.value(), ability, record.id, tick); !added) {
            return make_unexpected(added.error());
        }
    }
    return record.id;
}

Expected<GrantId, Error> AbilityRegistry::grant_ability(ecs::Entity owner, AbilityId ability,
                                                        Name source, i64 tick) noexcept {
    if (ability >= definitions_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such ability", 0});
    }
    auto row = ensure_row(owner);
    if (!row) {
        return make_unexpected(row.error());
    }
    GrantRecord record;
    record.id = next_grant_++;
    record.owner = owner;
    record.ability = ability;
    record.source = source;
    record.active = true;
    if (Status pushed = grants_.push_back(record); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (Status added = add_state(*row.value(), ability, record.id, tick); !added) {
        return make_unexpected(added.error());
    }
    return record.id;
}

bool AbilityRegistry::grant_covers(const GrantRecord& grant, AbilityId ability) const noexcept {
    if (grant.set == kInvalidAbilitySet) {
        return grant.ability == ability;
    }
    return std::ranges::any_of(sets_[grant.set].members,
                               [&](AbilityId member) { return member == ability; });
}

u32 AbilityRegistry::revoke(GrantId grant) noexcept {
    GrantRecord* record = nullptr;
    for (GrantRecord& candidate : grants_) {
        if (candidate.id == grant && candidate.active) {
            record = &candidate;
            break;
        }
    }
    if (record == nullptr) {
        return 0;
    }
    record->active = false;
    OwnerRow* row = row_of(record->owner);
    if (row == nullptr) {
        return 0;
    }
    u32 removed = 0;
    usize index = row->abilities.size();
    while (index-- > 0) {
        const AbilityId ability = row->abilities[index].ability;
        if (!grant_covers(*record, ability)) {
            continue;
        }
        // EXACT REMOVAL. An ability another live grant also provides stays; only what nothing else
        // grants goes. That is the requirement's "without affecting an ability granted by another
        // source", and it is why grants are records rather than a reference count.
        const bool granted_elsewhere = std::ranges::any_of(grants_, [&](const GrantRecord& other) {
            return other.active && other.id != grant && other.owner == record->owner &&
                   grant_covers(other, ability);
        });
        if (granted_elsewhere) {
            continue;
        }
        row->abilities.erase(index);
        ++removed;
    }
    return removed;
}

bool AbilityRegistry::has_ability(ecs::Entity owner, AbilityId ability) const noexcept {
    return state_of(owner, ability) != nullptr;
}

AbilityState* AbilityRegistry::state_of(ecs::Entity owner, AbilityId ability) noexcept {
    OwnerRow* row = row_of(owner);
    if (row == nullptr) {
        return nullptr;
    }
    for (AbilityState& state : row->abilities) {
        if (state.ability == ability) {
            return &state;
        }
    }
    return nullptr;
}

const AbilityState* AbilityRegistry::state_of(ecs::Entity owner, AbilityId ability) const noexcept {
    const OwnerRow* row = row_of(owner);
    if (row == nullptr) {
        return nullptr;
    }
    for (const AbilityState& state : row->abilities) {
        if (state.ability == ability) {
            return &state;
        }
    }
    return nullptr;
}

u32 AbilityRegistry::abilities_of(ecs::Entity owner, AbilityState* out,
                                  u32 capacity) const noexcept {
    const OwnerRow* row = row_of(owner);
    if (row == nullptr) {
        return 0;
    }
    u32 found = 0;
    for (const AbilityState& state : row->abilities) {
        if (out != nullptr && found < capacity) {
            out[found] = state;
        }
        ++found;
    }
    return found;
}

i64 AbilityRegistry::ready_tick(ecs::Entity owner, AbilityId ability) const noexcept {
    const AbilityState* state = state_of(owner, ability);
    if (state == nullptr) {
        return 0;
    }
    i64 ready = state->ready_tick;
    const TagId group = definitions_[ability].cooldown_group;
    if (group != kInvalidTag) {
        const OwnerRow* row = row_of(owner);
        for (usize index = 0; index < row->group_tags.size(); ++index) {
            if (row->group_tags[index] == group && row->group_ready[index] > ready) {
                ready = row->group_ready[index];
            }
        }
    }
    return ready;
}

bool AbilityRegistry::ready(ecs::Entity owner, AbilityId ability, i64 tick) const noexcept {
    const AbilityState* state = state_of(owner, ability);
    if (state == nullptr) {
        return false;
    }
    if (definitions_[ability].max_charges > 0 && state->charges == 0) {
        return false;
    }
    return tick >= ready_tick(owner, ability);
}

Status AbilityRegistry::start_cooldown(ecs::Entity owner, AbilityId ability, i64 tick) noexcept {
    AbilityState* state = state_of(owner, ability);
    if (state == nullptr) {
        return make_unexpected(Error{ErrorCode::NotFound, "that owner does not have it", 0});
    }
    const AbilityDefinition& definition = definitions_[ability];
    // A READY TICK, computed once. Nothing counts down and nothing accumulates, so a rollback to
    // tick N restores exactly the readiness tick N had.
    state->ready_tick = tick + definition.cooldown_ticks;
    if (definition.max_charges > 0 && state->charges > 0) {
        --state->charges;
        state->next_recharge_tick = tick + definition.recharge_ticks;
    }
    if (definition.cooldown_group == kInvalidTag) {
        return ok();
    }
    OwnerRow* row = row_of(owner);
    for (usize index = 0; index < row->group_tags.size(); ++index) {
        if (row->group_tags[index] == definition.cooldown_group) {
            row->group_ready[index] = state->ready_tick;
            return ok();
        }
    }
    if (Status pushed = row->group_tags.push_back(definition.cooldown_group); !pushed) {
        return pushed;
    }
    return row->group_ready.push_back(state->ready_tick);
}

u32 AbilityRegistry::advance_charges(i64 tick) noexcept {
    u32 recharged = 0;
    for (OwnerRow& row : states_) {
        for (AbilityState& state : row.abilities) {
            const AbilityDefinition& definition = definitions_[state.ability];
            if (definition.max_charges == 0 || definition.recharge_ticks <= 0) {
                continue;
            }
            while (state.charges < definition.max_charges && state.next_recharge_tick <= tick) {
                ++state.charges;
                state.next_recharge_tick += definition.recharge_ticks;
                ++recharged;
            }
        }
    }
    return recharged;
}

u64 AbilityRegistry::digest() const noexcept {
    u64 accumulated = 0;
    u32 counted = 0;
    for (const OwnerRow& row : states_) {
        u64 hash = mix(kFnvOffset, row.owner.bits());
        for (const AbilityState& state : row.abilities) {
            hash = mix(hash, state.ability);
            hash = mix(hash, static_cast<u64>(state.ready_tick));
            hash = mix(hash, state.charges);
        }
        accumulated += hash;
        ++counted;
    }
    return mix(mix(kFnvOffset, accumulated), counted);
}

}  // namespace cy::gameplay::abilities
