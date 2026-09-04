#pragma once
// Entity identity, generations, and the table that owns them. Task 2.1.
//
// `ecs-core` — "Entities": a 64-bit value packing a 32-bit index and a 32-bit generation, dense and
// recycled, with the generation incremented on destruction so a stale id is detectable.
//
// WHY THE GENERATION IS NOT OPTIONAL. An index alone is recycled the moment an entity dies, so a
// stale reference held by a projectile, a UI panel or a save file addresses whatever occupies the
// slot next. That failure has no diagnostic: the read succeeds and the data is somebody else's. The
// generation makes it a comparison, and `is_alive` a load and two compares.
//
// TWO GENERATION VALUES ARE RESERVED AND NEVER ISSUED.
//
//   0                     the null entity, so a default-constructed Entity is invalid rather than
//                         being index 0 of whatever world it is passed to.
//   kPlaceholderGeneration a command buffer's placeholder id (command_buffer.h). A structural
//                         change is deferred, so `create()` inside a system must hand back
//                         something usable *now* that is not yet an entity; making it a reserved
//                         generation means passing one to a live-world call is caught by the same
//                         `is_alive` check that catches a stale id, rather than by a separate rule
//                         nobody remembers.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/domain.h>

namespace cy::ecs {

/// The generation a command buffer's placeholder carries. Never issued by an `EntityTable`, so a
/// placeholder is never alive in any world.
inline constexpr u32 kPlaceholderGeneration = 0xFFFF'FFFFu;

/// An entity: an index into the world's table, and the generation that index was issued at.
///
/// Two `u32` members rather than one `u64` with shifts. The packing is the same eight bytes and the
/// same two fields; the difference is that `index()` is a load rather than a mask, and that a
/// debugger shows both halves without the reader doing the arithmetic. `bits()` is the flat
/// spelling, for the places that need one word — a command record, a hash, a serialized reference.
class Entity {
public:
    constexpr Entity() noexcept = default;

    [[nodiscard]] static constexpr Entity make(u32 index, u32 generation) noexcept {
        Entity entity;
        entity.index_ = index;
        entity.generation_ = generation;
        return entity;
    }

    [[nodiscard]] static constexpr Entity from_bits(u64 bits) noexcept {
        return make(static_cast<u32>(bits & 0xFFFF'FFFFu), static_cast<u32>(bits >> 32U));
    }

    [[nodiscard]] constexpr u32 index() const noexcept { return index_; }
    [[nodiscard]] constexpr u32 generation() const noexcept { return generation_; }

    /// True for anything that is not the null entity. It says nothing about liveness — only
    /// `World::is_alive` can, because only the world holds the table this generation is checked
    /// against.
    [[nodiscard]] constexpr bool valid() const noexcept { return generation_ != 0; }

    /// True for an id a command buffer handed out that has not been resolved yet.
    [[nodiscard]] constexpr bool placeholder() const noexcept {
        return generation_ == kPlaceholderGeneration;
    }

    [[nodiscard]] constexpr u64 bits() const noexcept {
        return static_cast<u64>(index_) | (static_cast<u64>(generation_) << 32U);
    }

    friend constexpr bool operator==(Entity, Entity) noexcept = default;

private:
    u32 index_ = 0;
    u32 generation_ = 0;
};

static_assert(sizeof(Entity) == 8, "an Entity is the 64-bit value ecs-core specifies");
static_assert(alignof(Entity) == 4, "the chunk key alignment follows from this");

/// The null entity, spelled so a comparison reads as one.
inline constexpr Entity kNoEntity{};

inline constexpr u32 kInvalidArchetype = 0xFFFF'FFFFu;

/// Where an entity's row is. The world's half of the ECS/storage split: `chunk_storage.h` knows a
/// chunk and a row, and this is what maps an entity onto one.
struct EntityLocation {
    u32 archetype = kInvalidArchetype;
    u32 chunk = 0;
    u32 row = 0;
};

/// The entity table: generations, locations, and the free list that makes indices dense.
///
/// Recycling is last-in-first-out and that is a determinism decision, not a performance one. The
/// order indices come back in is observable — it decides which chunk row a new entity lands in, and
/// therefore the order a query iterates — so it has to be a rule rather than whatever the container
/// happened to do. LIFO also reuses the most recently touched record, which is the one still in
/// cache.
class EntityTable {
public:
    explicit EntityTable(Allocator& allocator) noexcept
        : records_(allocator), free_indices_(allocator) {}

    [[nodiscard]] Expected<Entity, Error> create() noexcept;

    /// Mark an entity dead and return its index to the free list. The generation moves on, so every
    /// copy of the old id is detectably stale from here on.
    Status destroy(Entity entity) noexcept;

    [[nodiscard]] bool is_alive(Entity entity) const noexcept {
        const u32 index = entity.index();
        return index < records_.size() && records_[index].alive &&
               records_[index].generation == entity.generation();
    }

    /// The entity's row, or null when the id is stale. Null rather than an assertion: a stale id is
    /// a *runtime* condition — the entity died legitimately and somebody still holds the id — and
    /// `ecs-core` requires component access through one to return null.
    [[nodiscard]] const EntityLocation* location(Entity entity) const noexcept {
        return is_alive(entity) ? &records_[entity.index()].location : nullptr;
    }

    void set_location(Entity entity, const EntityLocation& location) noexcept;

    /// The entity currently occupying an index, whatever its generation. What a chunk's key column
    /// is validated against, and what a snapshot restores through.
    [[nodiscard]] Entity at(u32 index) const noexcept {
        return (index < records_.size()) ? Entity::make(index, records_[index].generation)
                                         : kNoEntity;
    }

    [[nodiscard]] u32 live_count() const noexcept { return live_; }
    /// Indices ever issued, live and free. The table's dense extent.
    [[nodiscard]] u32 capacity() const noexcept { return static_cast<u32>(records_.size()); }

    /// Reserve room for `wanted` indices, so a bulk creation allocates once rather than growing.
    [[nodiscard]] Status reserve(u32 wanted) noexcept { return records_.reserve(wanted); }

    void clear() noexcept;

    /// A record as a snapshot writes and reads it. Exposed because a snapshot restores the table
    /// verbatim — the same world, the same ids — and reconstructing it through `create()` would
    /// issue different generations.
    struct Record {
        u32 generation = 0;
        /// Liveness is its own flag rather than being read off the location. An entity exists
        /// before it has been placed in an archetype — that is the window `World::create` runs in —
        /// and a table that inferred liveness from a location could not represent it.
        bool alive = false;
        EntityLocation location;
    };

    [[nodiscard]] Span<const Record> records() const noexcept { return records_.span(); }
    [[nodiscard]] Span<const u32> free_indices() const noexcept { return free_indices_.span(); }
    [[nodiscard]] Status restore(Span<const Record> records, Span<const u32> free_indices,
                                 u32 live) noexcept;

private:
    /// The generation an index moves to when its entity dies. Skips both reserved values, so a
    /// table that has recycled four billion times still never issues 0 or a placeholder.
    [[nodiscard]] static u32 next_generation(u32 current) noexcept {
        const u32 advanced = current + 1;
        return (advanced == 0 || advanced == kPlaceholderGeneration) ? 1 : advanced;
    }

    Array<Record> records_;
    Array<u32> free_indices_;
    u32 live_ = 0;
};

}  // namespace cy::ecs
