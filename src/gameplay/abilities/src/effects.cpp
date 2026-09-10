// Effects, stacking and tick-exact periods. M8.b task 4.2.

#include <cy/gameplay/abilities/effects.h>

namespace cy::gameplay::abilities {
namespace {

constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;
constexpr i64 kNeverEnds = 0x7FFFFFFFFFFFFFFFLL;

[[nodiscard]] u64 mix(u64 hash, u64 value) noexcept {
    for (u32 byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8U)) & 0xFFULL;
        hash *= kFnvPrime;
    }
    return hash;
}

}  // namespace

const char* effect_kind_name(EffectKind kind) noexcept {
    switch (kind) {
        case EffectKind::Instant:
            return "Instant";
        case EffectKind::Duration:
            return "Duration";
        case EffectKind::Infinite:
            return "Infinite";
        case EffectKind::Periodic:
            return "Periodic";
        case EffectKind::Count:
            break;
    }
    return "Instant";
}

const char* stacking_policy_name(StackingPolicy policy) noexcept {
    switch (policy) {
        case StackingPolicy::Stack:
            return "Stack";
        case StackingPolicy::RefreshDuration:
            return "RefreshDuration";
        case StackingPolicy::Replace:
            return "Replace";
        case StackingPolicy::KeepHighest:
            return "KeepHighest";
        case StackingPolicy::KeepLowest:
            return "KeepLowest";
        case StackingPolicy::UniqueBySource:
            return "UniqueBySource";
        case StackingPolicy::LimitedStacks:
            return "LimitedStacks";
        case StackingPolicy::Count:
            break;
    }
    return "Stack";
}

const char* apply_outcome_name(ApplyOutcome outcome) noexcept {
    switch (outcome) {
        case ApplyOutcome::Applied:
            return "Applied";
        case ApplyOutcome::Refreshed:
            return "Refreshed";
        case ApplyOutcome::Stacked:
            return "Stacked";
        case ApplyOutcome::Replaced:
            return "Replaced";
        case ApplyOutcome::StacksCapped:
            return "StacksCapped";
        case ApplyOutcome::Kept:
            return "Kept";
        case ApplyOutcome::RefusedImmune:
            return "RefusedImmune";
        case ApplyOutcome::RefusedRequirement:
            return "RefusedRequirement";
        case ApplyOutcome::Count:
            break;
    }
    return "Applied";
}

EffectSystem::EffectSystem(Allocator& allocator, AttributeStore& attributes,
                           const TagRegistry& tag_registry) noexcept
    : allocator_(&allocator),
      attributes_(&attributes),
      tags_(&tag_registry),
      definitions_(allocator),
      modifiers_(allocator),
      instances_(allocator),
      free_slots_(allocator) {}

Expected<EffectDefId, Error> EffectSystem::declare(
    const EffectDefinition& definition, Span<const EffectModifier> modifiers,
    Span<const EffectModifier> period_modifiers) noexcept {
    if (definition.stable_id == 0) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "an effect's stable identity is never zero", 0});
    }
    if (find(definition.stable_id) != kInvalidEffectDef) {
        return make_unexpected(
            Error{ErrorCode::AlreadyExists, "that effect identity is declared", 0});
    }
    if (definition.kind == EffectKind::Periodic && definition.period_ticks <= 0) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "a periodic effect's period is a positive number of ticks",
                                     0});
    }
    EffectDefinition stored = definition;
    stored.first_modifier = static_cast<u32>(modifiers_.size());
    stored.modifier_count = static_cast<u32>(modifiers.size());
    if (Status appended = modifiers_.append(modifiers); !appended) {
        return make_unexpected(appended.error());
    }
    stored.first_period_modifier = static_cast<u32>(modifiers_.size());
    stored.period_modifier_count = static_cast<u32>(period_modifiers.size());
    if (Status appended = modifiers_.append(period_modifiers); !appended) {
        return make_unexpected(appended.error());
    }
    if (Status pushed = definitions_.push_back(stored); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<EffectDefId>(definitions_.size() - 1);
}

EffectDefId EffectSystem::find(u32 stable_id) const noexcept {
    for (usize index = 0; index < definitions_.size(); ++index) {
        if (definitions_[index].stable_id == stable_id) {
            return static_cast<EffectDefId>(index);
        }
    }
    return kInvalidEffectDef;
}

Status EffectSystem::reserve(u32 count) noexcept {
    return instances_.reserve(count);
}

i32 EffectSystem::find_existing(EffectDefId definition, ecs::Entity target,
                                ecs::Entity source) const noexcept {
    const bool by_source = definitions_[definition].stacking == StackingPolicy::UniqueBySource;
    for (usize index = 0; index < instances_.size(); ++index) {
        const EffectInstance& instance = instances_[index];
        if (!instance.active || instance.definition != definition || instance.target != target) {
            continue;
        }
        if (by_source && instance.source != source) {
            continue;
        }
        return static_cast<i32>(index);
    }
    return -1;
}

Expected<u32, Error> EffectSystem::allocate_instance() noexcept {
    if (!free_slots_.empty()) {
        const u32 slot = free_slots_[free_slots_.size() - 1];
        free_slots_.pop_back();
        return slot;
    }
    if (Status pushed = instances_.push_back(EffectInstance{}); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<u32>(instances_.size() - 1);
}

Status EffectSystem::bind_modifiers(const EffectDefinition& definition, u32 instance,
                                    ecs::Entity target, u16 stacks, u32& applied) noexcept {
    for (u32 index = 0; index < definition.modifier_count; ++index) {
        const EffectModifier& source = modifiers_[definition.first_modifier + index];
        Modifier modifier;
        modifier.attribute = source.attribute;
        modifier.op = source.op;
        // A stack multiplies an additive contribution and leaves the others alone: two stacks of
        // "+5 armour" is +10, and two stacks of "set armour to 0" is still 0.
        modifier.magnitude = source.op == ModifierOp::Add
                                 ? source.magnitude * static_cast<f32>(stacks)
                                 : source.magnitude;
        modifier.priority = source.priority;
        modifier.source_ordinal = definition.source_ordinal;
        modifier.effect_instance = instance;
        modifier.custom = source.custom;
        auto added = attributes_->add_modifier(target, modifier);
        if (!added) {
            return make_unexpected(added.error());
        }
        ++applied;
    }
    return ok();
}

void EffectSystem::unbind_modifiers(u32 instance, ecs::Entity target) noexcept {
    (void)attributes_->remove_modifiers_of(target, instance);
}

void EffectSystem::apply_period(const EffectDefinition& definition, ecs::Entity target,
                                u16 stacks) noexcept {
    // A period applies to the BASE value. A damage-over-time tick is a change that outlives the
    // effect; a held modifier is a change that ends with it, and conflating the two is how a
    // poison that stops ticking also un-damages its victim.
    for (u32 index = 0; index < definition.period_modifier_count; ++index) {
        const EffectModifier& source = modifiers_[definition.first_period_modifier + index];
        const f32 scaled = source.magnitude * static_cast<f32>(stacks);
        const f32 base = attributes_->base(target, source.attribute);
        f32 value = base;
        switch (source.op) {
            case ModifierOp::Add:
                value = base + scaled;
                break;
            case ModifierOp::Multiply:
                value = base * source.magnitude;
                break;
            case ModifierOp::Override:
                value = source.magnitude;
                break;
            case ModifierOp::ClampMin:
                value = base < source.magnitude ? source.magnitude : base;
                break;
            case ModifierOp::ClampMax:
                value = base > source.magnitude ? source.magnitude : base;
                break;
            case ModifierOp::Custom:
            case ModifierOp::Count:
                break;
        }
        (void)attributes_->set_base(target, source.attribute, value);
    }
}

Status EffectSystem::apply(EffectDefId definition_id, ecs::Entity target, ecs::Entity source,
                           i64 tick, const EntityTagStore* tags, ApplyReport& report) noexcept {
    report = ApplyReport{};
    if (definition_id >= definitions_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such effect definition", 0});
    }
    const EffectDefinition& definition = definitions_[definition_id];

    // 1 — IMMUNITY AND REQUIREMENT CHECKS, in that order, before anything is touched.
    if (tags != nullptr && definition.immunity_tag != kInvalidTag &&
        tags->has(*tags_, target, definition.immunity_tag)) {
        report.outcome = ApplyOutcome::RefusedImmune;
        return ok();
    }
    if (tags != nullptr && definition.required_tag != kInvalidTag &&
        !tags->has(*tags_, target, definition.required_tag)) {
        report.outcome = ApplyOutcome::RefusedRequirement;
        return ok();
    }

    // 2 — STACKING RESOLUTION. Declared, resolved here, never implemented per effect.
    // `Stack` creates another independent instance whatever is already there, and `Instant` holds
    // nothing — so neither needs the search, and skipping it is what keeps a bulk application of
    // one effect to a thousand targets linear rather than quadratic.
    const bool searches =
        definition.kind != EffectKind::Instant && definition.stacking != StackingPolicy::Stack;
    const i32 existing = searches ? find_existing(definition_id, target, source) : -1;
    if (existing >= 0) {
        EffectInstance& live = instances_[static_cast<usize>(existing)];
        const i64 end =
            definition.kind == EffectKind::Infinite ? kNeverEnds : tick + definition.duration_ticks;
        switch (definition.stacking) {
            case StackingPolicy::RefreshDuration:
                live.end_tick = end;
                report.outcome = ApplyOutcome::Refreshed;
                report.instance = live.id;
                report.stacks = live.stacks;
                return ok();
            case StackingPolicy::Replace:
                unbind_modifiers(live.id, target);
                live.start_tick = tick;
                live.end_tick = end;
                live.stacks = 1;
                live.source = source;
                live.next_period_tick =
                    definition.kind == EffectKind::Periodic ? tick + definition.period_ticks : end;
                if (Status bound = bind_modifiers(definition, live.id, target, live.stacks,
                                                  report.modifiers_applied);
                    !bound) {
                    return bound;
                }
                report.outcome = ApplyOutcome::Replaced;
                report.instance = live.id;
                report.stacks = live.stacks;
                return ok();
            case StackingPolicy::KeepHighest:
            case StackingPolicy::KeepLowest: {
                const bool keep_highest = definition.stacking == StackingPolicy::KeepHighest;
                const bool replace = keep_highest ? end > live.end_tick : end < live.end_tick;
                if (replace) {
                    live.end_tick = end;
                    live.source = source;
                    report.outcome = ApplyOutcome::Replaced;
                } else {
                    report.outcome = ApplyOutcome::Kept;
                }
                report.instance = live.id;
                report.stacks = live.stacks;
                return ok();
            }
            case StackingPolicy::LimitedStacks: {
                if (live.stacks >= definition.max_stacks) {
                    // The declared overflow behaviour: the duration refreshes and the count holds.
                    live.end_tick = end;
                    report.outcome = ApplyOutcome::StacksCapped;
                    report.instance = live.id;
                    report.stacks = live.stacks;
                    return ok();
                }
                unbind_modifiers(live.id, target);
                ++live.stacks;
                live.end_tick = end;
                if (Status bound = bind_modifiers(definition, live.id, target, live.stacks,
                                                  report.modifiers_applied);
                    !bound) {
                    return bound;
                }
                report.outcome = ApplyOutcome::Stacked;
                report.instance = live.id;
                report.stacks = live.stacks;
                return ok();
            }
            case StackingPolicy::UniqueBySource:
                live.end_tick = end;
                report.outcome = ApplyOutcome::Refreshed;
                report.instance = live.id;
                report.stacks = live.stacks;
                return ok();
            case StackingPolicy::Stack:
            case StackingPolicy::Count:
                break;
        }
    }

    // 3, 4 — MODIFIER APPLICATION AND ATTRIBUTE CHANGE.
    if (definition.kind == EffectKind::Instant) {
        // An instant effect changes the base and holds nothing: there is no instance to expire, so
        // there is no record to keep and nothing for a rollback to restore beyond the value itself.
        apply_period(definition, target, 1);
        for (u32 index = 0; index < definition.modifier_count; ++index) {
            const EffectModifier& modifier = modifiers_[definition.first_modifier + index];
            const f32 base = attributes_->base(target, modifier.attribute);
            const f32 value = modifier.op == ModifierOp::Multiply ? base * modifier.magnitude
                                                                  : base + modifier.magnitude;
            (void)attributes_->set_base(target, modifier.attribute, value);
            ++report.modifiers_applied;
        }
        report.outcome = ApplyOutcome::Applied;
        report.stacks = 1;
        return ok();
    }

    auto slot = allocate_instance();
    if (!slot) {
        return make_unexpected(slot.error());
    }
    EffectInstance& live = instances_[slot.value()];
    live.id = next_instance_++;
    live.definition = definition_id;
    live.target = target;
    live.source = source;
    live.start_tick = tick;
    live.end_tick =
        definition.kind == EffectKind::Infinite ? kNeverEnds : tick + definition.duration_ticks;
    live.next_period_tick =
        definition.kind == EffectKind::Periodic ? tick + definition.period_ticks : live.end_tick;
    live.stacks = 1;
    live.active = true;
    ++active_;
    if (Status bound =
            bind_modifiers(definition, live.id, target, live.stacks, report.modifiers_applied);
        !bound) {
        return bound;
    }
    report.outcome = ApplyOutcome::Applied;
    report.instance = live.id;
    report.stacks = live.stacks;
    return ok();
}

void EffectSystem::advance(i64 tick, EffectTickReport& report) noexcept {
    report = EffectTickReport{};
    for (usize index = 0; index < instances_.size(); ++index) {
        EffectInstance& live = instances_[index];
        if (!live.active) {
            continue;
        }
        ++report.instances_examined;
        const EffectDefinition& definition = definitions_[live.definition];
        if (definition.kind == EffectKind::Periodic) {
            // EXACT, AND EXACTLY AS MANY. The loop compares integers, so a period never lands
            // between two ticks and a rollback that re-runs a tick re-runs the same periods.
            while (live.next_period_tick <= tick && live.next_period_tick <= live.end_tick) {
                apply_period(definition, live.target, live.stacks);
                ++report.periods_applied;
                live.next_period_tick += definition.period_ticks;
            }
        }
        if (live.end_tick <= tick) {
            unbind_modifiers(live.id, live.target);
            report.modifiers_removed += definition.modifier_count;
            live.active = false;
            if (active_ > 0) {
                --active_;
            }
            ++report.expired;
            if (!free_slots_.push_back(static_cast<u32>(index)).has_value()) {
                // The slot is simply not reused; the instance stays inactive and is skipped. A
                // failed push here must not leave an expired effect looking live.
                continue;
            }
        }
    }
}

bool EffectSystem::remove(u32 instance) noexcept {
    for (usize index = 0; index < instances_.size(); ++index) {
        EffectInstance& live = instances_[index];
        if (!live.active || live.id != instance) {
            continue;
        }
        unbind_modifiers(live.id, live.target);
        live.active = false;
        if (active_ > 0) {
            --active_;
        }
        (void)free_slots_.push_back(static_cast<u32>(index));
        return true;
    }
    return false;
}

u32 EffectSystem::remove_all_on(ecs::Entity target) noexcept {
    u32 removed = 0;
    for (usize index = 0; index < instances_.size(); ++index) {
        EffectInstance& live = instances_[index];
        if (!live.active || live.target != target) {
            continue;
        }
        unbind_modifiers(live.id, live.target);
        live.active = false;
        if (active_ > 0) {
            --active_;
        }
        (void)free_slots_.push_back(static_cast<u32>(index));
        ++removed;
    }
    return removed;
}

const EffectInstance* EffectSystem::instance(u32 id) const noexcept {
    for (const EffectInstance& live : instances_) {
        if (live.active && live.id == id) {
            return &live;
        }
    }
    return nullptr;
}

u32 EffectSystem::active_on(ecs::Entity target, u32* out, u32 capacity) const noexcept {
    u32 found = 0;
    for (const EffectInstance& live : instances_) {
        if (!live.active || live.target != target) {
            continue;
        }
        if (out != nullptr && found < capacity) {
            out[found] = live.id;
        }
        ++found;
    }
    return found;
}

u32 EffectSystem::applied_by(ecs::Entity source, i64 tick, u32* out, u32 capacity) const noexcept {
    u32 found = 0;
    for (const EffectInstance& live : instances_) {
        if (!live.active || live.source != source || live.start_tick != tick) {
            continue;
        }
        if (out != nullptr && found < capacity) {
            out[found] = live.id;
        }
        ++found;
    }
    return found;
}

Status EffectSystem::restore(Span<const EffectInstance> instances) noexcept {
    instances_.clear();
    free_slots_.clear();
    active_ = 0;
    if (Status appended = instances_.append(instances); !appended) {
        return appended;
    }
    for (usize index = 0; index < instances_.size(); ++index) {
        if (instances_[index].active) {
            ++active_;
        } else if (!free_slots_.push_back(static_cast<u32>(index))) {
            return make_unexpected(
                Error{ErrorCode::OutOfMemory, "the effect free list could not be rebuilt", 0});
        }
    }
    return ok();
}

u64 EffectSystem::digest() const noexcept {
    u64 accumulated = 0;
    u32 counted = 0;
    for (const EffectInstance& live : instances_) {
        if (!live.active) {
            continue;
        }
        // Order-independent over the live set, and every field that decides an outcome is in it.
        u64 hash = mix(kFnvOffset, live.definition);
        hash = mix(hash, live.target.bits());
        hash = mix(hash, live.source.bits());
        hash = mix(hash, static_cast<u64>(live.start_tick));
        hash = mix(hash, static_cast<u64>(live.end_tick));
        hash = mix(hash, static_cast<u64>(live.next_period_tick));
        hash = mix(hash, live.stacks);
        accumulated += hash;
        ++counted;
    }
    return mix(mix(kFnvOffset, accumulated), counted);
}

}  // namespace cy::gameplay::abilities
