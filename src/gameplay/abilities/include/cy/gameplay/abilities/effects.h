#pragma once
// Effects, their stacking policies and their tick-exact periods. M8.b task 4.2.
//
// `gameplay-abilities-and-effects` — "Effects": effects "SHALL be authored as definitions and
// compiled, with runtime instances stored as **compact records** — effect identity, source, start
// and end tick, and stack count — **not as heap-allocated objects**". Kinds are instant, duration,
// infinite and periodic. "Periodic effects SHALL be scheduled on **simulation ticks**, not on
// accumulated floating-point time, so that periods are exact, reproducible, and rollback-safe."
// Application follows a defined pipeline: "immunity and requirement checks, stacking resolution,
// modifier application, attribute change, and event emission."
//
// And "Stacking policy": every effect declares one — "stack, refresh duration, replace, keep
// highest, keep lowest, unique by source, or limited stacks with a declared maximum" — and "Games
// SHALL NOT be required to implement stacking behaviour per effect."
//
// ================================================================================================
// WHY A TICK AND NOT A FLOAT, STATED ONCE
// ================================================================================================
//
// A float timer accumulates. Two peers stepping the same simulation at the same rate reach
// different accumulated values, and a rollback that replays three ticks does not land on the value
// it left. `EffectInstance` therefore holds `start_tick`, `end_tick` and `next_period_tick` as
// `i64`s, and `advance()` compares integers. The requirement's own measurement — "a periodic effect
// applies every thirty ticks for three hundred, exactly ten times, at exact ticks" — is an equality
// on integers here rather than an epsilon on floats.
//
// ================================================================================================
// AND WHY AN INSTANCE IS A PLAIN OLD RECORD
// ================================================================================================
//
// `EffectInstance` is trivially copyable and the live set is one array of them. Fifty thousand
// active effects are one allocation, a snapshot is a `memcpy`, and a rollback is an assignment.
// The forbidden-patterns list names "Heap-allocated effect instances on the normal path"; this is
// the shape that makes it structurally impossible rather than merely avoided.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/ecs/entity.h>
#include <cy/gameplay/abilities/attributes.h>
#include <cy/gameplay/tags.h>

namespace cy::gameplay::abilities {

enum class EffectKind : u8 {
    /// Applied once, to the base value, and never held.
    Instant = 0,
    Duration,
    Infinite,
    /// A duration effect that also applies its periodic modifiers every `period_ticks`.
    Periodic,
    Count,
};

const char* effect_kind_name(EffectKind kind) noexcept;

enum class StackingPolicy : u8 {
    /// Another independent instance.
    Stack = 0,
    /// One instance; a reapplication pushes its end tick out.
    RefreshDuration,
    /// One instance; a reapplication replaces it.
    Replace,
    KeepHighest,
    KeepLowest,
    /// One instance per source entity.
    UniqueBySource,
    /// One instance with a stack count, up to `max_stacks`.
    LimitedStacks,
    Count,
};

const char* stacking_policy_name(StackingPolicy policy) noexcept;

using EffectDefId = u32;
inline constexpr EffectDefId kInvalidEffectDef = 0xFFFFFFFFU;
inline constexpr u32 kInvalidEffectInstance = 0xFFFFFFFFU;

/// A modifier an effect applies, before it is bound to an instance.
struct EffectModifier {
    AttributeId attribute = kInvalidAttribute;
    ModifierOp op = ModifierOp::Add;
    f32 magnitude = 0.0F;
    u16 priority = 0;
    Name custom;
};

/// What an effect is, as authored and compiled. The tag fields are compact queries, not text.
struct EffectDefinition {
    Name name;
    u32 stable_id = 0;
    EffectKind kind = EffectKind::Instant;
    /// Ticks. Ignored by `Instant` and `Infinite`.
    i64 duration_ticks = 0;
    /// Ticks between periodic applications. Only `Periodic` reads it.
    i64 period_ticks = 0;
    StackingPolicy stacking = StackingPolicy::Stack;
    u16 max_stacks = 1;
    /// The target is immune while it carries this tag.
    TagId immunity_tag = kInvalidTag;
    /// The target must carry this tag for the effect to apply.
    TagId required_tag = kInvalidTag;
    /// The tag the effect grants its target while it lasts.
    TagId granted_tag = kInvalidTag;
    /// The declared ordinal used as the modifier tie-break's second term. Never a container index.
    u32 source_ordinal = 0;
    /// Where this definition's modifiers begin in the system's modifier table.
    u32 first_modifier = 0;
    u32 modifier_count = 0;
    /// The modifiers a `Periodic` application applies to the BASE value each period.
    u32 first_period_modifier = 0;
    u32 period_modifier_count = 0;
    PersistenceClass persistence = PersistenceClass::SessionTransient;
};

/// One live effect. **A compact record**: sixty-four bytes, trivially copyable, no pointer.
struct EffectInstance {
    u32 id = kInvalidEffectInstance;
    EffectDefId definition = kInvalidEffectDef;
    ecs::Entity target;
    ecs::Entity source;
    i64 start_tick = 0;
    /// The tick it stops being true. `INT64_MAX` for an infinite effect.
    i64 end_tick = 0;
    i64 next_period_tick = 0;
    u16 stacks = 1;
    bool active = false;
};

/// What one application did. Structured because "the outcome SHALL be reportable".
enum class ApplyOutcome : u8 {
    Applied = 0,
    /// The instance already there had its duration pushed out.
    Refreshed,
    /// The stack count rose.
    Stacked,
    Replaced,
    /// The stack limit was reached and the declared overflow behaviour applied.
    StacksCapped,
    /// The existing instance was kept: `KeepHighest` or `KeepLowest` said so.
    Kept,
    RefusedImmune,
    RefusedRequirement,
    Count,
};

const char* apply_outcome_name(ApplyOutcome outcome) noexcept;

struct ApplyReport {
    ApplyOutcome outcome = ApplyOutcome::Applied;
    u32 instance = kInvalidEffectInstance;
    u16 stacks = 0;
    /// Modifiers this application added to the attribute store.
    u32 modifiers_applied = 0;
};

/// What one `advance` did, so a caller can attribute cost and a test can assert exactness.
struct EffectTickReport {
    u32 instances_examined = 0;
    u32 periods_applied = 0;
    u32 expired = 0;
    u32 modifiers_removed = 0;
};

/// Compiled effect definitions and the live instances over them.
///
/// One instance array for everything: the requirement's "fifty thousand effects are compact records
/// processed in bulk" is a loop over this array, not a visit to fifty thousand objects.
class EffectSystem {
public:
    EffectSystem(Allocator& allocator, AttributeStore& attributes,
                 const TagRegistry& tag_registry) noexcept;

    EffectSystem(const EffectSystem&) = delete;
    EffectSystem& operator=(const EffectSystem&) = delete;

    /// Declare a definition. `modifiers` and `period_modifiers` are copied into the system's own
    /// tables, so a definition holds ranges rather than pointers and the whole set is one array.
    [[nodiscard]] Expected<EffectDefId, Error> declare(
        const EffectDefinition& definition, Span<const EffectModifier> modifiers,
        Span<const EffectModifier> period_modifiers = Span<const EffectModifier>()) noexcept;
    [[nodiscard]] EffectDefId find(u32 stable_id) const noexcept;
    [[nodiscard]] const EffectDefinition& definition(EffectDefId id) const noexcept {
        return definitions_[id];
    }
    [[nodiscard]] u32 definition_count() const noexcept {
        return static_cast<u32>(definitions_.size());
    }

    /// Make room for `count` live instances up front, so application allocates nothing.
    [[nodiscard]] Status reserve(u32 count) noexcept;

    /// The application pipeline, in the specification's order. `tags` is the target's tag set, for
    /// the immunity and requirement checks; null means neither can be tested and both pass.
    [[nodiscard]] Status apply(EffectDefId definition, ecs::Entity target, ecs::Entity source,
                               i64 tick, const EntityTagStore* tags, ApplyReport& report) noexcept;

    /// Fire every due period and expire everything whose end tick has passed. One pass over the
    /// instance array; no allocation.
    void advance(i64 tick, EffectTickReport& report) noexcept;

    /// Remove an instance early — a dispel, a cancellation.
    bool remove(u32 instance) noexcept;
    /// Remove every instance on an entity. What a despawn calls.
    u32 remove_all_on(ecs::Entity target) noexcept;

    [[nodiscard]] u32 active_count() const noexcept { return active_; }
    [[nodiscard]] u32 instance_capacity() const noexcept {
        return static_cast<u32>(instances_.size());
    }
    [[nodiscard]] const EffectInstance& instance_at(u32 index) const noexcept {
        return instances_[index];
    }
    [[nodiscard]] const EffectInstance* instance(u32 id) const noexcept;
    [[nodiscard]] u32 active_on(ecs::Entity target, u32* out, u32 capacity) const noexcept;
    /// The instances `source` applied at exactly `tick`. What a rejected prediction reverts, and
    /// the reason it is by SOURCE rather than by target: an activation's effects usually land on
    /// somebody else.
    [[nodiscard]] u32 applied_by(ecs::Entity source, i64 tick, u32* out,
                                 u32 capacity) const noexcept;

    /// The live set as bytes. A snapshot is this; a rollback is `restore`. Both are the array,
    /// which is what makes "all authoritative ability and effect state SHALL participate in
    /// snapshotting" cheap enough that nobody is tempted to leave it out.
    [[nodiscard]] Span<const EffectInstance> snapshot() const noexcept { return instances_.span(); }
    [[nodiscard]] Status restore(Span<const EffectInstance> instances) noexcept;

    /// A value identity over the live set, in a canonical order.
    [[nodiscard]] u64 digest() const noexcept;

private:
    [[nodiscard]] i32 find_existing(EffectDefId definition, ecs::Entity target,
                                    ecs::Entity source) const noexcept;
    [[nodiscard]] Expected<u32, Error> allocate_instance() noexcept;
    [[nodiscard]] Status bind_modifiers(const EffectDefinition& definition, u32 instance,
                                        ecs::Entity target, u16 stacks, u32& applied) noexcept;
    void unbind_modifiers(u32 instance, ecs::Entity target) noexcept;
    void apply_period(const EffectDefinition& definition, ecs::Entity target, u16 stacks) noexcept;

    Allocator* allocator_;
    AttributeStore* attributes_;
    const TagRegistry* tags_;
    Array<EffectDefinition> definitions_;
    Array<EffectModifier> modifiers_;
    Array<EffectInstance> instances_;
    Array<u32> free_slots_;
    u32 next_instance_ = 0;
    u32 active_ = 0;
};

}  // namespace cy::gameplay::abilities
