#pragma once
// Ability definitions, sets, grants, per-owner state, costs and cooldowns. M8.b tasks 4.1 and 4.2.
//
// `gameplay-abilities-and-effects` — "Compiled ability programs": an ability is "authored as a
// **definition asset** and compiled into an **ability program** shared by every owner that has it",
// resolving "tag requirements into compact queries, attribute references into bindings, constant
// expressions, cost and cooldown descriptions, targeting configuration, and the effects applied —
// producing a program executed without graph traversal or reflection at activation time. **There
// SHALL NOT be one polymorphic runtime object per ability per owner.**"
//
// "Ability state and sets": per-owner state is "**compact and ECS-native**: an ability identity,
// cooldown state, charges, and any small declared state". Sets are "shared by templates, so that a
// unit type's abilities are referenced rather than duplicated per instance". Abilities are
// "grantable and revocable at runtime … through grant records, so that the source of a granted
// ability is known and removal is exact."
//
// "Costs, cooldowns, and charges": cost handling is "**transactional**: validate, reserve, and
// commit — so that two activations resolved in the same tick cannot both spend the same resource".
// "**Cooldowns SHALL be expressed as ticks**, as a ready-tick value rather than a counting float
// timer", and may be shared through a cooldown group identified by a gameplay tag.
//
// ================================================================================================
// WHERE THE COMPILED PROGRAM COMES FROM, AND WHY IT IS NOT COMPILED HERE
// ================================================================================================
//
// M8.b's spike found that abilities and visual scripting are one language serving two consumers —
// `visual-scripting` says so itself: it "SHALL additionally provide gameplay and **ability** graph
// languages, lowering to ECS systems and **ability programs** respectively". So the compiler is
// `cy::graph::script::compile_ability`, which produces a `ScriptProgram` plus the pipeline's stage
// table, and this module holds what it operates on: the attributes, the effects, the costs, the
// cooldowns and the per-owner state. Writing a second compiler here would be the duplication the
// spike measured against.
//
// `AbilityDefinition::program` is therefore a pointer to a program somebody else compiled, and it
// may be null: an ability whose whole content is "cost, cooldown, effects" needs no graph, and
// making one mandatory would put a compiler in the path of the simplest thing the module does.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/core/values/name.h>
#include <cy/ecs/entity.h>
#include <cy/gameplay/abilities/effects.h>
#include <cy/gameplay/abilities/targeting.h>
#include <cy/gameplay/tags.h>
#include <cy/graph/lower_script.h>

namespace cy::gameplay::abilities {

using AbilityId = u32;
inline constexpr AbilityId kInvalidAbility = 0xFFFFFFFFU;
using AbilitySetId = u32;
inline constexpr AbilitySetId kInvalidAbilitySet = 0xFFFFFFFFU;
using GrantId = u32;
inline constexpr GrantId kInvalidGrant = 0xFFFFFFFFU;

/// How a client may treat an activation before the authority answers.
enum class PredictionPolicy : u8 {
    None = 0,
    ClientPredicted,
    AuthorityOnly,
    DeterministicLockstep,
    Count,
};

const char* prediction_policy_name(PredictionPolicy policy) noexcept;

/// What an activation spends.
enum class CostKind : u8 {
    /// An attribute's current value — mana, stamina.
    Attribute = 0,
    /// One of the ability's own charges.
    Charge,
    /// A project resource service, named. This module does not implement one; the ledger reports
    /// the requirement and the host answers it.
    Resource,
    Count,
};

struct Cost {
    CostKind kind = CostKind::Attribute;
    AttributeId attribute = kInvalidAttribute;
    f32 amount = 0.0F;
    Name resource;
};

/// What an agent needs to reason about an ability without knowing which ability it is.
/// `gameplay-abilities-and-effects`: "Abilities MAY declare optional planning metadata — range,
/// expected effect magnitude, resource cost, and role tags — so that an agent can reason about them
/// without hard-coded knowledge."
struct PlanningMetadata {
    f32 range = 0.0F;
    f32 expected_magnitude = 0.0F;
    f32 resource_cost = 0.0F;
    TagId role_tag = kInvalidTag;
};

/// An ability, compiled. Every reference here is an identifier or a compact query; nothing is
/// resolved by name at activation time.
struct AbilityDefinition {
    Name name;
    u32 stable_id = 0;
    /// Ticks. A ready-tick value is computed from it; this is never counted down.
    i64 cooldown_ticks = 0;
    /// The cooldown group, as a gameplay tag. `kInvalidTag` means the ability's own cooldown.
    TagId cooldown_group = kInvalidTag;
    u16 max_charges = 0;
    i64 recharge_ticks = 0;
    TagId required_tag = kInvalidTag;
    TagId forbidden_tag = kInvalidTag;
    PredictionPolicy prediction = PredictionPolicy::AuthorityOnly;
    TargetRules targeting;
    PlanningMetadata planning;
    /// Ranges into the registry's own tables. A definition holds no allocation.
    u32 first_cost = 0;
    u32 cost_count = 0;
    u32 first_effect = 0;
    u32 effect_count = 0;
    /// The compiled graph program, or null. See the header comment.
    const graph::script::AbilityProgram* program = nullptr;
};

/// One owner's state for one ability. **Compact**: an identity, a ready tick and a charge count.
struct AbilityState {
    AbilityId ability = kInvalidAbility;
    /// The tick it becomes usable. A VALUE, not a countdown — exact under rollback and identical
    /// across peers, which a float accumulator is not.
    i64 ready_tick = 0;
    u16 charges = 0;
    i64 next_recharge_tick = 0;
    /// The grant that put it here, so revocation is exact.
    GrantId grant = kInvalidGrant;
};

/// Where a granted ability came from. "the source of a granted ability is known and removal is
/// exact".
struct GrantRecord {
    GrantId id = kInvalidGrant;
    ecs::Entity owner;
    /// The set granted, or `kInvalidAbilitySet` for a single ability.
    AbilitySetId set = kInvalidAbilitySet;
    AbilityId ability = kInvalidAbility;
    /// What granted it: an equipment name, an upgrade, a feature.
    Name source;
    bool active = false;
};

/// One reservation held between validate and commit.
struct CostReservation {
    ecs::Entity owner;
    AttributeId attribute = kInvalidAttribute;
    f32 amount = 0.0F;
    u64 activation = 0;
};

/// Transactional costs. `gameplay-abilities-and-effects`: "two activations resolved in the same
/// tick cannot both spend the same resource."
///
/// A reservation is held from `reserve()` to `commit()` or `release()`, and `available()` subtracts
/// what is reserved from what the attribute says. Two activations in one tick therefore see
/// different answers, which is the whole content of the requirement — a non-transactional path
/// would let both read the same value and both spend it.
class CostLedger {
public:
    CostLedger(Allocator& allocator, AttributeStore& attributes) noexcept;

    CostLedger(const CostLedger&) = delete;
    CostLedger& operator=(const CostLedger&) = delete;

    [[nodiscard]] f32 available(ecs::Entity owner, AttributeId attribute) const noexcept;
    [[nodiscard]] Status reserve(ecs::Entity owner, AttributeId attribute, f32 amount,
                                 u64 activation) noexcept;
    /// Spend everything reserved for `activation` and drop the reservations.
    [[nodiscard]] Status commit(u64 activation) noexcept;
    /// Drop the reservations without spending. What a refused activation calls.
    void release(u64 activation) noexcept;
    [[nodiscard]] u32 outstanding() const noexcept {
        return static_cast<u32>(reservations_.size());
    }

private:
    AttributeStore* attributes_;
    Array<CostReservation> reservations_;
};

/// Definitions, sets, grants and per-owner state.
class AbilityRegistry {
public:
    explicit AbilityRegistry(Allocator& allocator) noexcept;

    AbilityRegistry(const AbilityRegistry&) = delete;
    AbilityRegistry& operator=(const AbilityRegistry&) = delete;

    [[nodiscard]] Expected<AbilityId, Error> declare(const AbilityDefinition& definition,
                                                     Span<const Cost> costs,
                                                     Span<const EffectDefId> effects) noexcept;
    [[nodiscard]] AbilityId find(u32 stable_id) const noexcept;
    [[nodiscard]] const AbilityDefinition& definition(AbilityId id) const noexcept {
        return definitions_[id];
    }
    [[nodiscard]] u32 definition_count() const noexcept {
        return static_cast<u32>(definitions_.size());
    }
    [[nodiscard]] Span<const Cost> costs_of(AbilityId id) const noexcept;
    [[nodiscard]] Span<const EffectDefId> effects_of(AbilityId id) const noexcept;

    /// A set shared by every owner of a template. "they SHALL reference one ability set, not carry
    /// a copy each."
    [[nodiscard]] Expected<AbilitySetId, Error> create_set(Name name) noexcept;
    [[nodiscard]] Status add_to_set(AbilitySetId set, AbilityId ability) noexcept;
    [[nodiscard]] Span<const AbilityId> set_members(AbilitySetId set) const noexcept;
    [[nodiscard]] u32 set_count() const noexcept { return static_cast<u32>(sets_.size()); }

    /// Grant a whole set, or one ability. The grant record is what makes revocation exact.
    [[nodiscard]] Expected<GrantId, Error> grant_set(ecs::Entity owner, AbilitySetId set,
                                                     Name source, i64 tick) noexcept;
    [[nodiscard]] Expected<GrantId, Error> grant_ability(ecs::Entity owner, AbilityId ability,
                                                         Name source, i64 tick) noexcept;
    /// Withdraw exactly that grant. An ability granted twice by two sources survives one removal.
    u32 revoke(GrantId grant) noexcept;

    [[nodiscard]] bool has_ability(ecs::Entity owner, AbilityId ability) const noexcept;
    [[nodiscard]] AbilityState* state_of(ecs::Entity owner, AbilityId ability) noexcept;
    [[nodiscard]] const AbilityState* state_of(ecs::Entity owner, AbilityId ability) const noexcept;
    [[nodiscard]] u32 abilities_of(ecs::Entity owner, AbilityState* out,
                                   u32 capacity) const noexcept;
    [[nodiscard]] u32 owner_count() const noexcept { return static_cast<u32>(states_.size()); }

    // --- Cooldowns. Ticks, never a counting float. ----------------------------------------------

    /// Is the ability ready at `tick`? Reads the ability's own ready tick and its group's.
    [[nodiscard]] bool ready(ecs::Entity owner, AbilityId ability, i64 tick) const noexcept;
    /// The tick it becomes ready, which is what an interface shows and what a peer compares.
    [[nodiscard]] i64 ready_tick(ecs::Entity owner, AbilityId ability) const noexcept;
    [[nodiscard]] Status start_cooldown(ecs::Entity owner, AbilityId ability, i64 tick) noexcept;
    /// Recharge charges whose recharge tick has passed. One pass, no allocation.
    u32 advance_charges(i64 tick) noexcept;

    /// A value identity over every owner's ability state.
    [[nodiscard]] u64 digest() const noexcept;

private:
    struct AbilitySet {
        AbilitySetId id = kInvalidAbilitySet;
        Name name;
        Array<AbilityId> members;
    };
    struct OwnerRow {
        ecs::Entity owner;
        Array<AbilityState> abilities;
        /// Ready ticks per cooldown group, so a shared cooldown is one value rather than a copy on
        /// each member ability.
        Array<TagId> group_tags;
        Array<i64> group_ready;
    };

    [[nodiscard]] OwnerRow* row_of(ecs::Entity owner) noexcept;
    [[nodiscard]] const OwnerRow* row_of(ecs::Entity owner) const noexcept;
    [[nodiscard]] Expected<OwnerRow*, Error> ensure_row(ecs::Entity owner) noexcept;
    [[nodiscard]] Status add_state(OwnerRow& row, AbilityId ability, GrantId grant,
                                   i64 tick) noexcept;
    /// Does this grant provide that ability — directly, or through the set it granted?
    [[nodiscard]] bool grant_covers(const GrantRecord& grant, AbilityId ability) const noexcept;

    Allocator* allocator_;
    Array<AbilityDefinition> definitions_;
    Array<Cost> costs_;
    Array<EffectDefId> effects_;
    Array<AbilitySet> sets_;
    Array<GrantRecord> grants_;
    Array<OwnerRow> states_;
    /// Owner -> its row. The same reason `AttributeStore` keeps one: a hundred thousand owners
    /// with ability sets is an architectural target, and a scan per lookup makes it quadratic.
    HashMap<u64, u32> index_;
    GrantId next_grant_ = 0;
};

}  // namespace cy::gameplay::abilities
