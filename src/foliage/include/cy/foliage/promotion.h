#pragma once
// Promotion: an instance becomes an entity when gameplay touches it, and comes back when it stops
// mattering. M10 task 2.4.
//
// `foliage` — "Promotion to entities": "A foliage instance SHALL be PROMOTABLE to an ECS entity
// when gameplay requires interaction — damage, felling, physics, attachment, or scripted behaviour
// — and DEMOTABLE back to an instance when it no longer does. Promotion SHALL preserve IDENTITY ...
// Demotion SHALL be STATE-PRESERVING OR REFUSED ... Promotion and demotion SHALL be bounded per
// frame by a budget, and the number of promoted instances SHALL be REPORTABLE."
//
// ================================================================================================
// THIS MODULE DOES NOT CREATE THE ENTITY
// ================================================================================================
//
// `promote()` produces a `PromotionOrder`: the identity, the decoded transform, the species and the
// cause. The caller creates the entity — with whatever components its game needs — and calls
// `bind()` with the handle. Demotion is the mirror: `demote()` either produces a
// `DemotionOrder` naming the entity to destroy and the state to fold back, or REFUSES with a
// reason.
//
// That is the same discipline `src/terrain/` follows with physics and navigation, and it is picked
// for the same reason: a subsystem that reached into the ECS while a cell was streaming is the
// re-entrancy `world-partition-and-streaming` forbids in as many words. It also means
// `cy::foliage` needs `cy::ecs` for one 8-byte handle type and for nothing else — there is no
// `World&` anywhere in this module.
//
// ================================================================================================
// WHY THE SUPPRESSION IS A FLAG ON THE INSTANCE AND NOT A REMOVAL FROM THE BLOCK
// ================================================================================================
//
// "the GPU instance SHALL be SUPPRESSED so it is not drawn twice." Removing the instance from its
// species block would renumber every slot after it, and a slot is what an identity is derived from
// (instance.h) — so felling one tree would rename every tree after it in the cluster, orphan every
// exception anchored to them, and do it silently. `InstanceFlags::kPromoted` costs one bit and
// keeps every other identity fixed.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/quat.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/ecs/entity.h>
#include <cy/foliage/exceptions.h>
#include <cy/foliage/instance.h>
#include <cy/foliage/species.h>

namespace cy::foliage {

/// Why an instance was promoted. `foliage`'s diagnostics requirement: "WHEN promoted instance count
/// is high THEN the diagnostics SHALL report WHAT PROMOTED THEM."
enum class PromotionCause : u8 {
    Damage = 0,
    Felling,
    Physics,
    Attachment,
    Script,
    kCount,
};

inline constexpr u32 kPromotionCauseCount = static_cast<u32>(PromotionCause::kCount);

[[nodiscard]] const char* promotion_cause_name(PromotionCause cause) noexcept;

/// Why a promotion was refused.
enum class PromotionProblem : u8 {
    None = 0,
    /// No such cluster is resident.
    UnknownCluster,
    /// No instance with that identity in that cluster.
    UnknownInstance,
    /// The species declares `promotable = false`. A grass blade has no gameplay surface, and a
    /// promotion that created an entity for one would be an entity nothing can interact with.
    SpeciesNotPromotable,
    /// Already promoted. Idempotent rather than an error would hide a double-promote that means a
    /// caller lost track of a handle.
    AlreadyPromoted,
    /// The per-frame promotion budget is spent.
    BudgetExhausted,
    /// The instance was removed by an exception. There is nothing standing there.
    InstanceRemoved,
};

[[nodiscard]] const char* promotion_problem_name(PromotionProblem problem) noexcept;

/// Why a demotion was refused. `foliage` — "An instance whose state cannot be represented in the
/// compact instance form — MID-FALL, PARTIALLY DESTRUCTED, PHYSICALLY CONSTRAINED, or CARRYING
/// GAMEPLAY STATE — SHALL remain an entity rather than losing that state."
///
/// The four the specification names, each as its own reason so a diagnostic can say which.
enum class DemotionRefusal : u8 {
    None = 0,
    MidFall,
    PartiallyDestructed,
    PhysicallyConstrained,
    CarriesGameplayState,
    /// The per-frame demotion budget is spent. Not a refusal of the demotion — it will be retried.
    BudgetExhausted,
    NotPromoted,
};

[[nodiscard]] const char* demotion_refusal_name(DemotionRefusal refusal) noexcept;

/// What the caller tells this module about an entity it is asking to demote. Every member is a
/// thing the compact instance form CANNOT represent, so the answer is a function of these and of
/// nothing hidden.
struct DemotionState {
    /// The entity is falling. `foliage`'s own scenario.
    bool mid_fall = false;
    /// Pieces of it are gone.
    bool partially_destructed = false;
    /// A joint, a rope, a vehicle attachment.
    bool constrained = false;
    /// Gameplay hung state on it — a quest marker, a stash, an owner.
    bool gameplay_state = false;
    /// It is lying down and stays lying down. REPRESENTABLE: `InstanceFlags::kFelled` plus an
    /// exception, which is what makes "a felled tree stays felled" work across the round trip.
    bool felled = false;
    /// It is gone entirely — burned to nothing, harvested. Representable as `kRemoved`.
    bool destroyed = false;
};

/// What the caller must do to promote. Produced by `promote()`; this module creates nothing.
struct PromotionOrder {
    InstanceId identity;
    ClusterId cluster;
    u32 slot = 0;
    SpeciesId species;
    /// Absolute position, decoded from the cluster's quantisation.
    world::WorldVec3d position;
    Quat rotation;
    f32 scale = 1.0F;
    u8 variation = 0;
    u8 age = 0;
    PromotionCause cause = PromotionCause::Damage;
};

/// What the caller must do to demote: destroy this entity. The state it folds back is already
/// applied to the cluster and recorded as an exception by `demote()` itself, because a demotion
/// whose state landed only if the caller remembered to ask would be a demotion that loses state.
struct DemotionOrder {
    InstanceId identity;
    ClusterId cluster;
    u32 slot = 0;
    ecs::Entity entity;
    /// Whether an exception was recorded. False where the instance returned unchanged.
    bool recorded_exception = false;
};

/// One promoted instance, as the registry holds it.
struct PromotedInstance {
    InstanceId identity;
    ClusterId cluster;
    u32 slot = 0;
    SpeciesId species;
    ecs::Entity entity;
    PromotionCause cause = PromotionCause::Damage;
    /// The tick, frame or sequence the promotion happened at. The caller's own clock; this module
    /// only stores and reports it.
    u64 stamp = 0;
};

/// `foliage` — "Promotion and demotion SHALL be bounded per frame by a budget", and "Promotion
/// count SHALL be BUDGETED SEPARATELY, since promoted instances cost simulation rather than
/// rendering."
struct PromotionBudget {
    /// Promotions allowed to start this frame.
    u32 promotions_per_frame = 8;
    /// Demotions allowed this frame.
    u32 demotions_per_frame = 8;
    /// The ceiling on how many may be promoted at once. Reaching it refuses further promotions,
    /// which is a visible, reportable state rather than an unbounded simulation cost.
    u32 max_promoted = 256;
};

/// The counts `foliage`'s diagnostics requirement asks for.
struct PromotionDiagnostics {
    u32 promoted = 0;
    u32 by_cause[kPromotionCauseCount] = {};
    u32 promotions_this_frame = 0;
    u32 demotions_this_frame = 0;
    u32 refused_budget = 0;
    u32 refused_state = 0;
    u32 refused_ceiling = 0;
};

/// Who is promoted, and the two transitions.
///
/// It holds no `ecs::World`, creates nothing and destroys nothing — see the header note.
class PromotionRegistry {
public:
    PromotionRegistry(Allocator& allocator, const SpeciesLibrary& library, u64 seed) noexcept;

    PromotionRegistry(const PromotionRegistry&) = delete;
    PromotionRegistry& operator=(const PromotionRegistry&) = delete;

    void set_budget(const PromotionBudget& budget) noexcept { budget_ = budget; }
    [[nodiscard]] const PromotionBudget& budget() const noexcept { return budget_; }

    /// Reset the per-frame halves of the budget. Called once per frame by the owner.
    void begin_frame() noexcept;

    /// Ask to promote one instance. Suppresses the GPU instance on success (`kPromoted`), so the
    /// suppression cannot be forgotten by a caller that creates the entity and then crashes.
    [[nodiscard]] Expected<PromotionOrder, Error> promote(FoliageCluster& cluster,
                                                          InstanceId identity, PromotionCause cause,
                                                          u64 stamp) noexcept;

    /// Bind the entity the caller created to the order it was given. Until this is called the
    /// instance is promoted-but-unbound, which `unbound()` reports — a state that means a caller
    /// took an order and dropped it.
    [[nodiscard]] Status bind(InstanceId identity, ecs::Entity entity) noexcept;

    /// Ask to demote. State-preserving or refused, per the specification.
    ///
    /// On success the cluster's flags are updated and — where the state needs one — an exception is
    /// recorded, so "a felled tree stays felled" is true whether or not the caller does anything
    /// else. `exceptions` may be null only when `state` carries nothing to record.
    [[nodiscard]] Expected<DemotionOrder, Error> demote(FoliageCluster& cluster,
                                                        InstanceId identity,
                                                        const DemotionState& state,
                                                        ExceptionStore* exceptions,
                                                        u64 sequence) noexcept;

    /// Why the last demotion was refused. Valid until the next `demote()`.
    [[nodiscard]] DemotionRefusal last_refusal() const noexcept { return refusal_; }
    /// Why the last promotion was refused. Valid until the next `promote()`.
    [[nodiscard]] PromotionProblem last_problem() const noexcept { return problem_; }

    [[nodiscard]] const PromotedInstance* find(InstanceId identity) const noexcept;
    [[nodiscard]] Span<const PromotedInstance> promoted() const noexcept {
        return promoted_.span();
    }
    [[nodiscard]] usize size() const noexcept { return promoted_.size(); }
    /// Promoted instances with no entity bound yet.
    [[nodiscard]] u32 unbound() const noexcept;

    [[nodiscard]] const PromotionDiagnostics& diagnostics() const noexcept { return diagnostics_; }

    /// Whether the state can be folded back into sixteen bytes. Public because it is the whole of
    /// the "state-preserving or refused" decision and a test should be able to ask it directly
    /// without driving a cluster.
    [[nodiscard]] static DemotionRefusal representable(const DemotionState& state) noexcept;

private:
    [[nodiscard]] PromotedInstance* find_mutable(InstanceId identity) noexcept;

    Allocator* allocator_;
    const SpeciesLibrary* library_;
    u64 seed_ = 0;
    PromotionBudget budget_;
    Array<PromotedInstance> promoted_;
    HashMap<u64, usize> index_;
    PromotionDiagnostics diagnostics_;
    DemotionRefusal refusal_ = DemotionRefusal::None;
    PromotionProblem problem_ = PromotionProblem::None;
};

}  // namespace cy::foliage
