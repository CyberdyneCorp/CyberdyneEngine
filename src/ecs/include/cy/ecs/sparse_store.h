#pragma once
// The side table a sparse component lives in. Part of task 2.2.
//
// `ecs-core`: "adding or removing a sparse component SHALL NOT change the entity's archetype". That
// is the whole of its reason to exist — a component present on under ~1 % of entities and toggled
// every frame would otherwise move its entity between two archetypes twice a frame, and each move
// is a row copy plus two location updates.
//
// The shape is M1's `SparseSet` with the value type erased: a dense array of values, a dense array
// of keys, and a sparse index from an entity's index to its dense position. The generation is kept
// beside the key so that a stale entity id reads as absent rather than as the slot's new occupant —
// the same rule the entity table enforces, restated here because this table outlives an entity's
// row.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/entity.h>

namespace cy::ecs {

class SparseStore {
public:
    SparseStore(Allocator& allocator, u32 value_size) noexcept
        : sparse_(allocator),
          keys_(allocator),
          generations_(allocator),
          values_(allocator),
          value_size_(value_size) {}

    /// Insert or overwrite the value for `entity`. The bytes are copied; a null `value` inserts a
    /// zeroed entry, which is how a caller that only wants presence spells it.
    [[nodiscard]] Status set(Entity entity, const void* value) noexcept;

    /// The entity's value, or null when it has none or the id is stale.
    [[nodiscard]] void* find(Entity entity) noexcept;
    [[nodiscard]] const void* find(Entity entity) const noexcept;

    /// Remove the entity's entry. `NotFound` when it had none.
    Status erase(Entity entity) noexcept;

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(keys_.size()); }
    [[nodiscard]] u32 value_size() const noexcept { return value_size_; }

    /// The dense arrays, for serialization and for a system that wants to walk the set itself.
    [[nodiscard]] Span<const u32> keys() const noexcept { return keys_.span(); }
    [[nodiscard]] Span<const u32> generations() const noexcept { return generations_.span(); }
    [[nodiscard]] Span<const u8> values() const noexcept { return values_.span(); }

    void clear() noexcept;

private:
    static constexpr u32 kAbsent = 0xFFFF'FFFFu;

    [[nodiscard]] u32 position_of(Entity entity) const noexcept;

    Array<u32> sparse_;
    Array<u32> keys_;
    Array<u32> generations_;
    Array<u8> values_;
    u32 value_size_;
};

}  // namespace cy::ecs
