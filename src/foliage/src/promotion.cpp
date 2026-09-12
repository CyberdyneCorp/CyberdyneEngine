// Promotion and demotion: orders out, entity handles in, and the four refusals the specification
// names. See promotion.h for why this module creates no entity and why suppression is a flag.

#include <cy/foliage/promotion.h>

#include <cy/core/math/scalar.h>

namespace cy::foliage {

const char* promotion_cause_name(PromotionCause cause) noexcept {
    switch (cause) {
        case PromotionCause::Damage:
            return "damage";
        case PromotionCause::Felling:
            return "felling";
        case PromotionCause::Physics:
            return "physics";
        case PromotionCause::Attachment:
            return "attachment";
        case PromotionCause::Script:
            return "script";
        case PromotionCause::kCount:
            break;
    }
    return "unknown";
}

const char* promotion_problem_name(PromotionProblem problem) noexcept {
    switch (problem) {
        case PromotionProblem::None:
            return "none";
        case PromotionProblem::UnknownCluster:
            return "unknown-cluster";
        case PromotionProblem::UnknownInstance:
            return "unknown-instance";
        case PromotionProblem::SpeciesNotPromotable:
            return "species-not-promotable";
        case PromotionProblem::AlreadyPromoted:
            return "already-promoted";
        case PromotionProblem::BudgetExhausted:
            return "budget-exhausted";
        case PromotionProblem::InstanceRemoved:
            return "instance-removed";
    }
    return "unknown";
}

const char* demotion_refusal_name(DemotionRefusal refusal) noexcept {
    switch (refusal) {
        case DemotionRefusal::None:
            return "none";
        case DemotionRefusal::MidFall:
            return "mid-fall";
        case DemotionRefusal::PartiallyDestructed:
            return "partially-destructed";
        case DemotionRefusal::PhysicallyConstrained:
            return "physically-constrained";
        case DemotionRefusal::CarriesGameplayState:
            return "carries-gameplay-state";
        case DemotionRefusal::BudgetExhausted:
            return "budget-exhausted";
        case DemotionRefusal::NotPromoted:
            return "not-promoted";
    }
    return "unknown";
}

PromotionRegistry::PromotionRegistry(Allocator& allocator, const SpeciesLibrary& library,
                                     u64 seed) noexcept
    : allocator_(&allocator),
      library_(&library),
      seed_(seed),
      promoted_(allocator),
      index_(allocator) {}

void PromotionRegistry::begin_frame() noexcept {
    diagnostics_.promotions_this_frame = 0;
    diagnostics_.demotions_this_frame = 0;
}

DemotionRefusal PromotionRegistry::representable(const DemotionState& state) noexcept {
    // The specification's own four, in its own order. Each is a state the sixteen-byte record
    // cannot carry, so demoting would LOSE it — and `foliage` says losing it is worse than the
    // memory the entity costs.
    if (state.mid_fall) {
        return DemotionRefusal::MidFall;
    }
    if (state.partially_destructed) {
        return DemotionRefusal::PartiallyDestructed;
    }
    if (state.constrained) {
        return DemotionRefusal::PhysicallyConstrained;
    }
    if (state.gameplay_state) {
        return DemotionRefusal::CarriesGameplayState;
    }
    return DemotionRefusal::None;
}

PromotedInstance* PromotionRegistry::find_mutable(InstanceId identity) noexcept {
    usize* slot = index_.find(identity.value);
    return slot == nullptr ? nullptr : &promoted_[*slot];
}

const PromotedInstance* PromotionRegistry::find(InstanceId identity) const noexcept {
    const usize* slot = index_.find(identity.value);
    return slot == nullptr ? nullptr : &promoted_[*slot];
}

u32 PromotionRegistry::unbound() const noexcept {
    u32 total = 0;
    for (const PromotedInstance& promoted : promoted_) {
        total += promoted.entity == ecs::kNoEntity ? 1U : 0U;
    }
    return total;
}

Expected<PromotionOrder, Error> PromotionRegistry::promote(FoliageCluster& cluster,
                                                           InstanceId identity,
                                                           PromotionCause cause,
                                                           u64 stamp) noexcept {
    problem_ = PromotionProblem::None;
    if (find(identity) != nullptr) {
        problem_ = PromotionProblem::AlreadyPromoted;
        return fail(ErrorCode::AlreadyExists, "this instance is already promoted");
    }
    if (diagnostics_.promotions_this_frame >= budget_.promotions_per_frame) {
        problem_ = PromotionProblem::BudgetExhausted;
        ++diagnostics_.refused_budget;
        return fail(ErrorCode::Unavailable, "the per-frame promotion budget is spent");
    }
    if (promoted_.size() >= budget_.max_promoted) {
        problem_ = PromotionProblem::BudgetExhausted;
        ++diagnostics_.refused_ceiling;
        return fail(ErrorCode::Unavailable, "the promoted-instance ceiling has been reached");
    }
    const u32 slot = cluster.slot_of(seed_, identity);
    if (slot == FoliageCluster::kNoSlot) {
        problem_ = PromotionProblem::UnknownInstance;
        return fail(ErrorCode::NotFound, "no instance with that identity in this cluster");
    }
    const FoliageInstance* instance = cluster.at(slot);
    if (instance == nullptr) {
        problem_ = PromotionProblem::UnknownInstance;
        return fail(ErrorCode::NotFound, "no instance at that slot");
    }
    if (instance->flags.has(InstanceFlags::kRemoved)) {
        problem_ = PromotionProblem::InstanceRemoved;
        return fail(ErrorCode::NotFound, "the instance was removed by an exception");
    }
    const SpeciesId species = cluster.species_at(instance->species_slot);
    const SpeciesDeclaration* declaration = library_->find(species);
    if (declaration == nullptr || !declaration->promotable) {
        problem_ = PromotionProblem::SpeciesNotPromotable;
        return fail(ErrorCode::Unsupported,
                    "this species declares no gameplay surface and may not be promoted");
    }

    PromotionOrder order;
    order.identity = identity;
    order.cluster = cluster.id();
    order.slot = slot;
    order.species = species;
    order.position = cluster.bounds().decode(*instance);
    const f32 yaw = static_cast<f32>(instance->yaw) * (math::kTwoPi / 65536.0F);
    order.rotation = Quat::from_euler_yxz(Vec3{0.0F, yaw, 0.0F});
    order.scale =
        declaration->scale_min + ((declaration->scale_max - declaration->scale_min) *
                                  (static_cast<f32>(instance->scale) * (1.0F / 65535.0F)));
    order.variation = instance->variation;
    order.age = instance->age;
    order.cause = cause;

    // Suppression happens HERE and not after the caller creates the entity: a caller that took an
    // order and then failed would otherwise leave the instance drawn AND simulated.
    InstanceFlags flags = instance->flags;
    flags.set(InstanceFlags::kPromoted);
    if (Status set = cluster.set_flags(slot, flags); !set) {
        return make_unexpected(set.error());
    }

    PromotedInstance record;
    record.identity = identity;
    record.cluster = cluster.id();
    record.slot = slot;
    record.species = species;
    record.entity = ecs::kNoEntity;
    record.cause = cause;
    record.stamp = stamp;
    if (Status pushed = promoted_.push_back(record); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (Expected<usize*, Error> placed = index_.insert(identity.value, promoted_.size() - 1);
        !placed) {
        promoted_.pop_back();
        return fail(placed.error().code, placed.error().message);
    }
    ++diagnostics_.promotions_this_frame;
    ++diagnostics_.by_cause[static_cast<u32>(cause)];
    diagnostics_.promoted = static_cast<u32>(promoted_.size());
    return order;
}

Status PromotionRegistry::bind(InstanceId identity, ecs::Entity entity) noexcept {
    PromotedInstance* record = find_mutable(identity);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "no promotion order outstanding for that identity");
    }
    record->entity = entity;
    return ok();
}

Expected<DemotionOrder, Error> PromotionRegistry::demote(FoliageCluster& cluster,
                                                         InstanceId identity,
                                                         const DemotionState& state,
                                                         ExceptionStore* exceptions,
                                                         u64 sequence) noexcept {
    refusal_ = DemotionRefusal::None;
    const PromotedInstance* record = find(identity);
    if (record == nullptr) {
        refusal_ = DemotionRefusal::NotPromoted;
        return fail(ErrorCode::NotFound, "that instance is not promoted");
    }
    if (diagnostics_.demotions_this_frame >= budget_.demotions_per_frame) {
        refusal_ = DemotionRefusal::BudgetExhausted;
        ++diagnostics_.refused_budget;
        return fail(ErrorCode::Unavailable, "the per-frame demotion budget is spent");
    }
    if (const DemotionRefusal refusal = representable(state); refusal != DemotionRefusal::None) {
        refusal_ = refusal;
        ++diagnostics_.refused_state;
        return fail(ErrorCode::Unsupported, demotion_refusal_name(refusal));
    }

    const u32 slot = record->slot;
    const FoliageInstance* instance = cluster.at(slot);
    if (instance == nullptr) {
        refusal_ = DemotionRefusal::NotPromoted;
        return fail(ErrorCode::NotFound, "the instance's slot no longer exists");
    }

    DemotionOrder order;
    order.identity = identity;
    order.cluster = record->cluster;
    order.slot = slot;
    order.entity = record->entity;

    InstanceFlags flags = instance->flags;
    flags.clear(InstanceFlags::kPromoted);
    if (state.destroyed) {
        flags.set(InstanceFlags::kRemoved);
    }
    if (state.felled) {
        flags.set(InstanceFlags::kFelled);
    }
    if (state.destroyed || state.felled) {
        flags.set(InstanceFlags::kException);
    }
    if (Status set = cluster.set_flags(slot, flags); !set) {
        return make_unexpected(set.error());
    }

    // "A FELLED TREE STAYS FELLED": the exception is recorded HERE, not by the caller. A demotion
    // whose state landed only if the caller remembered to ask would be a demotion that loses state
    // exactly as often as a caller forgets — which is the failure the requirement is about.
    if (state.destroyed || state.felled) {
        if (exceptions == nullptr) {
            return fail(ErrorCode::InvalidArgument,
                        "demoting an instance whose state must be recorded needs an exception "
                        "store: without one the state would be silently lost");
        }
        FoliageException exception;
        exception.kind = state.destroyed ? ExceptionKind::Removed : ExceptionKind::Modified;
        exception.identity = identity;
        exception.cluster = record->cluster;
        exception.position = cluster.bounds().decode(*instance);
        exception.species = record->species;
        exception.state = flags;
        exception.authored = false;
        exception.sequence = sequence;
        if (Status recorded = exceptions->record(exception); !recorded) {
            return make_unexpected(recorded.error());
        }
        order.recorded_exception = true;
    }

    // Swap-remove from the registry, repairing the moved record's index entry.
    const usize* found = index_.find(identity.value);
    if (found != nullptr) {
        const usize position = *found;
        const usize last = promoted_.size() - 1;
        if (position != last) {
            promoted_[position] = promoted_[last];
            if (usize* moved = index_.find(promoted_[position].identity.value); moved != nullptr) {
                *moved = position;
            }
        }
        promoted_.pop_back();
        (void)index_.remove(identity.value);
    }
    ++diagnostics_.demotions_this_frame;
    diagnostics_.promoted = static_cast<u32>(promoted_.size());
    return order;
}

}  // namespace cy::foliage
