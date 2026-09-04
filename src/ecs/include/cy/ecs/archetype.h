#pragma once
// Archetypes: the unique component set on an entity, and the chunks its entities live in. Task 2.3.
//
// `ecs-core` — "Archetypes and chunk storage". THE STORAGE IS M1'S AND THIS FILE DOES NOT ALLOCATE
// (design.md §1). Every chunk comes from `<cy/core/memory/chunk_storage.h>`, whose `ChunkAllocator`
// is under M1's budget tree; an ECS with a chunk allocator of its own would be a second accounting
// the memory-pressure system cannot see, and M6's residency policy holds its allocations from the
// first one. What this file adds is the meaning M1 deliberately does not have:
//
//   a ChunkLayout's columns  <-  an archetype's component set
//   a ColumnSpec             <-  a component type's size and alignment
//   the key                  <-  the Entity
//   a column's version       <-  a component's change version
//
// TWO PROPERTIES WORTH KNOWING BEFORE READING THE CODE.
//
// `ChunkStore` IS NEITHER COPYABLE NOR MOVABLE — the deleted copy constructor suppresses the
// implicit move — so archetypes cannot live in an `Array<Archetype>`: the first growth would not
// compile, and if it did it would move chunks out from under every location record in the entity
// table. `ArchetypeTable` therefore holds pointers to individually allocated archetypes, whose
// addresses are stable for the life of the world.
//
// COLUMN ORDER IS THE COMPONENT-ID ORDER, NOT THE ORDER THE CALLER LISTED. `ecs-core` requires that
// two entities given the same components in different orders end up in the same archetype; sorting
// the set is where that is made true, and it is why the mask — which has no order — is the identity
// and the sorted list is only its expansion.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/chunk_storage.h>
#include <cy/ecs/component.h>
#include <cy/ecs/entity.h>

namespace cy::ecs {

/// A shared component's value on one archetype: which component, and which interned value.
struct SharedValue {
    ComponentTypeId component = kInvalidComponent;
    /// An index into the world's interning table for that component type. Values are interned so
    /// that archetype identity is an integer comparison rather than a memcmp of the payload.
    u32 value = 0;

    friend constexpr bool operator==(SharedValue, SharedValue) noexcept = default;
};

/// One archetype: a component set, its chunk layout, and the chunks holding its entities.
class Archetype {
public:
    /// Build an archetype for a component set. `components` must be sorted ascending and free of
    /// duplicates — `ArchetypeTable` is what guarantees that, and it is the only caller.
    [[nodiscard]] static Expected<Archetype*, Error> create(Allocator& allocator, u32 id,
                                                            const ComponentMask& mask,
                                                            Span<const ComponentTypeId> components,
                                                            Span<const SharedValue> shared,
                                                            const ComponentRegistry& registry,
                                                            u32 chunk_bytes) noexcept;

    static void destroy(Allocator& allocator, Archetype* archetype) noexcept;

    ~Archetype();

    Archetype(const Archetype&) = delete;
    Archetype& operator=(const Archetype&) = delete;
    Archetype(Archetype&&) = delete;
    Archetype& operator=(Archetype&&) = delete;

    [[nodiscard]] u32 id() const noexcept { return id_; }
    [[nodiscard]] const ComponentMask& mask() const noexcept { return mask_; }
    [[nodiscard]] Span<const ComponentTypeId> components() const noexcept {
        return components_.span();
    }
    [[nodiscard]] Span<const SharedValue> shared() const noexcept { return shared_.span(); }

    /// The chunk column holding `component`, or -1 when this archetype does not store one — either
    /// because it does not have the component, or because the component's kind has no column (a tag
    /// is presence only; a shared value is per archetype; a sparse component is in a side table).
    ///
    /// A binary search over at most `kMaxChunkColumns` entries. A query resolves it once per
    /// archetype and iterates from a span; only random access pays it per call, which is the cost
    /// `ecs-core` documents random access as having.
    [[nodiscard]] i32 column_of(ComponentTypeId component) const noexcept;

    [[nodiscard]] ChunkStore& store() noexcept { return *store_; }
    [[nodiscard]] const ChunkStore& store() const noexcept { return *store_; }
    [[nodiscard]] const ChunkLayout& layout() const noexcept { return store_->layout(); }
    [[nodiscard]] u32 chunk_count() const noexcept { return store_->chunk_count(); }
    [[nodiscard]] ChunkView chunk(u32 index) noexcept { return store_->chunk(index); }
    [[nodiscard]] u64 entity_count() const noexcept { return store_->row_count(); }

    /// Rows one chunk of this archetype holds, derived by `ChunkLayout` from the component set.
    [[nodiscard]] u32 capacity() const noexcept { return store_->layout().capacity(); }

    /// A contiguous run of freshly added rows in one chunk.
    struct RowRange {
        u32 chunk = 0;
        u32 first_row = 0;
        u32 count = 0;
    };

    /// Add one row. The row's component bytes are zeroed, so a component whose value is never
    /// written reads as zero rather than as whatever the previous occupant left.
    [[nodiscard]] Expected<ChunkStore::RowLocation, Error> add_row(u64 version) noexcept;

    /// Add `count` rows, reported as runs. One `ChunkStore::add_row` per chunk rather than per row:
    /// `ecs-core` requires bulk creation to allocate into chunks in bulk without a per-entity
    /// archetype lookup, and a per-row store call is measurably the cost that requirement is about.
    [[nodiscard]] Status add_rows(u32 count, u64 version, Array<RowRange>& out) noexcept;

    /// Remove a row, releasing anything its buffer components own first. Reports which row was
    /// moved into the gap so the caller can fix the moved entity's location record.
    ChunkStore::RowMove remove_row(u32 chunk, u32 row) noexcept;

    /// Remove a row whose buffer components have already been handed to another archetype.
    ///
    /// The distinction is ownership, not efficiency. `copy_shared_columns` copies a buffer's header
    /// — its size, capacity and heap pointer — so after a transition the heap block has exactly one
    /// owner and it is the target row. Releasing here as well would free memory the target is still
    /// pointing at.
    ChunkStore::RowMove remove_moved_row(u32 chunk, u32 row) noexcept {
        return store_->remove_row(chunk, row);
    }

    /// Release what a row's buffer components own, except for those whose component is in `keep`.
    /// The other half of a transition: a buffer component the target archetype does not have is
    /// being dropped, and nothing else will free it.
    void release_buffers_except(u32 chunk, u32 row, const ComponentMask& keep) noexcept;

    /// A pointer to one row's component value, or null when this archetype has no column for it.
    [[nodiscard]] void* value_at(u32 chunk, u32 row, ComponentTypeId component) noexcept;

    /// Copy every column the two archetypes share from one row to another. What an archetype
    /// transition is: a memcpy per shared column, and nothing per field.
    void copy_shared_columns(Archetype& source, u32 source_chunk, u32 source_row, u32 target_chunk,
                             u32 target_row) noexcept;

    /// Release what row's buffer components own, without removing the row. Used when a row's data
    /// has already been moved elsewhere and must not be freed twice.
    void release_buffers(u32 chunk, u32 row) noexcept;

    /// Zero one row's bytes in every column. A fresh row's contract.
    void clear_row(u32 chunk, u32 row) noexcept;

    /// Empty every chunk, releasing what the rows' buffer components own. The chunks themselves are
    /// kept: a snapshot restore refills them, and giving them back only to reacquire them would
    /// churn M1's allocator for nothing. `trim()` is what actually returns them.
    void clear_rows(u64 version) noexcept;

    /// Stamp every column of a chunk with `version`. A row added or removed changes every column of
    /// the chunk it lands in, and change detection is chunk-granular by specification.
    void stamp(u32 chunk, u64 version) noexcept;

    [[nodiscard]] Allocator& allocator() const noexcept { return *allocator_; }

private:
    Archetype(Allocator& allocator, u32 id) noexcept;

    Allocator* allocator_;
    u32 id_;
    ComponentMask mask_;
    Array<ComponentTypeId> components_;
    /// The subset of `components_` that has a column, in column order. Parallel to the layout's
    /// columns, so `column_of` is a search over this and nothing else.
    Array<ComponentTypeId> columns_;
    /// The columns whose component owns memory outside the chunk. Empty for almost every archetype,
    /// which is what makes the release pass a check of `empty()` rather than a walk.
    Array<u32> releasing_columns_;
    Array<ComponentReleaseFn> releasers_;
    Array<SharedValue> shared_;
    /// Heap-allocated because `ChunkStore` is neither copyable nor movable; see the header.
    ChunkStore* store_ = nullptr;
};

/// Every archetype in one world, and the lookup from a component set to one.
///
/// Archetypes are never destroyed while the world lives. A query caches the archetypes it matches
/// by index and updates that list incrementally as new ones appear (query.h); destroying one would
/// make every cached index a dangling reference, and an empty archetype costs a layout and an empty
/// chunk store. `trim()` gives the chunks back without removing the archetype.
class ArchetypeTable {
public:
    ArchetypeTable(Allocator& allocator, u32 chunk_bytes) noexcept
        : allocator_(&allocator), archetypes_(allocator), chunk_bytes_(chunk_bytes) {}

    ~ArchetypeTable();

    ArchetypeTable(const ArchetypeTable&) = delete;
    ArchetypeTable& operator=(const ArchetypeTable&) = delete;

    /// The archetype for this component set and these shared values, created if it is new.
    /// `components` may be in any order; it is sorted here, which is where "component addition
    /// order does not matter" is made true.
    [[nodiscard]] Expected<Archetype*, Error> find_or_create(const ComponentMask& mask,
                                                             Span<const ComponentTypeId> components,
                                                             Span<const SharedValue> shared,
                                                             const ComponentRegistry& registry);

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(archetypes_.size()); }
    [[nodiscard]] Archetype& at(u32 index) noexcept { return *archetypes_[index]; }
    [[nodiscard]] const Archetype& at(u32 index) const noexcept { return *archetypes_[index]; }

    /// Give every empty chunk back to the allocator, across every archetype. The pressure response.
    usize trim() noexcept;

    void clear() noexcept;

private:
    Allocator* allocator_;
    Array<Archetype*> archetypes_;
    u32 chunk_bytes_;
};

}  // namespace cy::ecs
