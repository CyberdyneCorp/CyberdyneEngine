#pragma once
// Attributes and modifiers. M8.b task 4.2.
//
// `gameplay-abilities-and-effects` — "Attributes": attributes are "**typed ECS data** with declared
// metadata — base value, clamps, replication, persistence, prediction behaviour, and presentation
// information — **not entries in a string-keyed dictionary**", and "Attribute access in hot paths
// SHALL be through compiled bindings; string lookup SHALL NOT occur per access."
//
// And "Modifiers and evaluation order": add, multiply, override, clamp minimum, clamp maximum and a
// declared custom operation; "**The evaluation order SHALL be specified by the engine, not
// conventional**: modifiers apply in a defined sequence — additive, then multiplicative, then
// override, then clamping — with equal-priority modifiers ordered by a stable tie-break. Order
// SHALL NOT depend on insertion order, container iteration order, or the order effects happened to
// be applied, since those differ between machines and between runs. The final value and every
// contributing modifier SHALL be inspectable."
//
// ================================================================================================
// THE ORDER, STATED ONCE, IN FULL, SO THAT TWO MACHINES CANNOT DISAGREE
// ================================================================================================
//
//   1. the declared base value
//   2. every Add, summed
//   3. every Multiply, multiplied
//   4. every Custom, in tie-break order, each `value = f(value, magnitude)`
//   5. the Override of highest priority, if any — it replaces everything above it
//   6. every ClampMin, then every ClampMax
//   7. the attribute's own declared clamps, last, because a declared clamp is the attribute's range
//      and a modifier may not widen it
//
// THE TIE-BREAK WITHIN A CLASS is `(priority descending, source ordinal ascending, modifier
// identity ascending)`. All three are declared values assigned by the caller; none is a container
// position and none is a pointer. `add_modifier` inserts into that order rather than appending, so
// the evaluation walk is a single forward pass and the order cannot be a property of when the
// insert happened.
//
// ================================================================================================
// AND WHY THE VALUE IS CACHED RATHER THAN WALKED PER ACCESS
// ================================================================================================
//
// "Attribute evaluation SHALL be structured so that an entity with many modifiers is not evaluated
// by walking a linked structure per access." A row holds its evaluated values and a dirty mask;
// adding or removing a modifier dirties one attribute, and the next read recomputes that one. A
// read of an unchanged attribute is an indexed load.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/core/values/name.h>
#include <cy/ecs/entity.h>
#include <cy/gameplay/fragments.h>

namespace cy::gameplay::abilities {

/// An attribute's dense runtime identity. The compiled binding an ability holds — **never a
/// string**, which is what keeps string lookup off the hot path.
using AttributeId = u32;
inline constexpr AttributeId kInvalidAttribute = 0xFFFFFFFFU;

/// How a client may treat an attribute before the authority answers.
enum class PredictionBehaviour : u8 {
    /// Never predicted; the client waits.
    None = 0,
    /// The client may apply its own change and reconcile.
    Predicted,
    /// Predicted, and a mismatch is corrected silently rather than reported.
    PredictedSoftCorrect,
    Count,
};

/// What an attribute is, as declared. One declaration; replication, saving and hashing follow from
/// it, exactly as a session fragment's do.
struct AttributeDeclaration {
    Name name;
    /// Persistent identity assigned by the project; zero is null.
    u32 stable_id = 0;
    f32 base = 0.0F;
    f32 minimum = 0.0F;
    f32 maximum = 0.0F;
    bool has_minimum = false;
    bool has_maximum = false;
    bool replicated = true;
    PersistenceClass persistence = PersistenceClass::SessionTransient;
    PredictionBehaviour prediction = PredictionBehaviour::None;
    /// Presentation: what an interface calls it. Never the runtime identity.
    Name display_name;
};

/// The attributes a project declares. Composable per entity type: a set names the subset a unit
/// type carries, so "a unit carries the attributes it has and nothing more".
class AttributeSchema {
public:
    explicit AttributeSchema(Allocator& allocator) noexcept;

    AttributeSchema(const AttributeSchema&) = delete;
    AttributeSchema& operator=(const AttributeSchema&) = delete;

    [[nodiscard]] Expected<AttributeId, Error> declare(
        const AttributeDeclaration& declaration) noexcept;
    /// By stable identity. Cook time and tooling only.
    [[nodiscard]] AttributeId find(u32 stable_id) const noexcept;
    /// By name. **Cook time only** — this is the string lookup the requirement keeps off the hot
    /// path, and it is spelled `find_by_name` so that its appearance in a profile is obvious.
    [[nodiscard]] AttributeId find_by_name(Name name) const noexcept;
    [[nodiscard]] u32 count() const noexcept { return static_cast<u32>(declarations_.size()); }
    [[nodiscard]] const AttributeDeclaration& declaration(AttributeId id) const noexcept {
        return declarations_[id];
    }

    /// Does replication carry it? Does a save? Derived from the declaration, like a fragment's.
    [[nodiscard]] bool replicated(AttributeId id) const noexcept;
    [[nodiscard]] bool saved(AttributeId id) const noexcept;

private:
    Array<AttributeDeclaration> declarations_;
};

/// The operations a modifier may perform.
enum class ModifierOp : u8 {
    Add = 0,
    Multiply,
    /// A project's own, resolved through `AttributeStore::register_custom`.
    Custom,
    Override,
    ClampMin,
    ClampMax,
    Count,
};

const char* modifier_op_name(ModifierOp op) noexcept;

using ModifierId = u32;
inline constexpr ModifierId kInvalidModifier = 0xFFFFFFFFU;

/// One modifier, as applied. Every field of the tie-break is here and none of them is a position.
struct Modifier {
    AttributeId attribute = kInvalidAttribute;
    ModifierOp op = ModifierOp::Add;
    f32 magnitude = 0.0F;
    /// Higher wins. Declared by the effect, not by when it landed.
    u16 priority = 0;
    /// The declared ordinal of whatever applied this — an effect definition's identity, an
    /// equipment slot, a rule index. The second half of the tie-break.
    u32 source_ordinal = 0;
    /// The effect instance that owns it, so an effect's expiry removes exactly its own.
    u32 effect_instance = 0;
    /// The custom operation's name, for `ModifierOp::Custom`.
    Name custom;
};

/// One modifier's part in the final value, for the inspector.
struct Contribution {
    ModifierId modifier = kInvalidModifier;
    ModifierOp op = ModifierOp::Add;
    f32 magnitude = 0.0F;
    u16 priority = 0;
    u32 source_ordinal = 0;
    /// The running value after this modifier was applied.
    f32 value_after = 0.0F;
    /// False for an Override that lost to a higher-priority one, or a modifier the walk skipped.
    bool applied = true;
};

/// A project's own modifier operation.
using CustomModifierFn = f32 (*)(f32 current, f32 magnitude) noexcept;

/// Attribute values and their modifiers, per entity.
class AttributeStore {
public:
    AttributeStore(Allocator& allocator, const AttributeSchema& schema) noexcept;

    AttributeStore(const AttributeStore&) = delete;
    AttributeStore& operator=(const AttributeStore&) = delete;

    [[nodiscard]] Status add_entity(ecs::Entity entity) noexcept;
    void remove_entity(ecs::Entity entity) noexcept;
    [[nodiscard]] bool has_entity(ecs::Entity entity) const noexcept;
    [[nodiscard]] u32 entity_count() const noexcept { return static_cast<u32>(rows_.size()); }
    [[nodiscard]] ecs::Entity entity_at(u32 index) const noexcept { return rows_[index].entity; }

    [[nodiscard]] Status set_base(ecs::Entity entity, AttributeId attribute, f32 value) noexcept;
    [[nodiscard]] f32 base(ecs::Entity entity, AttributeId attribute) const noexcept;
    /// The evaluated value, in the order at the top of this file.
    [[nodiscard]] f32 current(ecs::Entity entity, AttributeId attribute) const noexcept;

    [[nodiscard]] Expected<ModifierId, Error> add_modifier(ecs::Entity entity,
                                                           const Modifier& modifier) noexcept;
    bool remove_modifier(ecs::Entity entity, ModifierId modifier) noexcept;
    /// Remove every modifier an effect instance applied. What an expiry calls.
    u32 remove_modifiers_of(ecs::Entity entity, u32 effect_instance) noexcept;
    [[nodiscard]] u32 modifier_count(ecs::Entity entity) const noexcept;

    [[nodiscard]] Status register_custom(Name name, CustomModifierFn function) noexcept;

    /// The base and every contributing modifier, in evaluation order. "Contributions are visible."
    [[nodiscard]] u32 explain(ecs::Entity entity, AttributeId attribute, Contribution* out,
                              u32 capacity) const noexcept;

    /// A value identity over every attribute of every entity, in a canonical order. What a
    /// determinism hash folds in, and what two peers compare.
    [[nodiscard]] u64 digest() const noexcept;

private:
    struct ModifierRecord {
        ModifierId id = kInvalidModifier;
        Modifier modifier;
    };
    struct Row {
        ecs::Entity entity;
        Array<f32> base;
        /// The evaluated value and its dirty bit. `mutable` because recomputing a cache is not a
        /// change to what the store holds: `current()` answers the same question either way, and a
        /// const read that had to be non-const would push the cache out to every caller.
        mutable Array<f32> cached;
        mutable Array<u8> dirty;
        Array<ModifierRecord> modifiers;
    };
    struct Custom {
        Name name;
        CustomModifierFn function = nullptr;
    };

    [[nodiscard]] Row* find(ecs::Entity entity) noexcept;
    [[nodiscard]] const Row* find(ecs::Entity entity) const noexcept;
    [[nodiscard]] f32 evaluate(const Row& row, AttributeId attribute, Contribution* out,
                               u32 capacity, u32& written) const noexcept;
    [[nodiscard]] CustomModifierFn custom_of(Name name) const noexcept;
    /// Is `a` ordered before `b` under the declared tie-break?
    [[nodiscard]] static bool orders_before(const ModifierRecord& a,
                                            const ModifierRecord& b) noexcept;

    Allocator* allocator_;
    const AttributeSchema* schema_;
    Array<Row> rows_;
    /// Entity -> its row. A linear scan here is the difference between "a hundred thousand
    /// entities carrying attribute sets" and a quadratic loop that looks fine at a hundred.
    HashMap<u64, u32> index_;
    Array<Custom> customs_;
    ModifierId next_modifier_ = 0;
};

}  // namespace cy::gameplay::abilities
