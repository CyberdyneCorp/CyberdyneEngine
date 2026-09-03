#pragma once
// Chunked structure-of-arrays storage. Task 2.6, design.md §7.
//
// `core-memory-and-containers` — "Chunked component storage": fixed-size chunks (default 16 KiB)
// holding structure-of-arrays data for one archetype, with the key array first and each column
// aligned to its type's alignment; chunk capacity derived from the column set and recorded; a
// per-column change version so change detection is O(1) per chunk; and linear iteration with no
// per-element indirection.
//
// THIS FILE KNOWS NOTHING ABOUT COMPONENTS, ARCHETYPES OR QUERIES, AND MUST NOT LEARN. design.md §7
// puts chunked storage here rather than in the ECS precisely so that `ecs-core` is about entities
// and archetypes rather than about memory, and M2 builds the ECS on top of this. The vocabulary is
// therefore deliberately one level down:
//
//   an archetype's component set   ->  a ChunkLayout's columns
//   a component type               ->  a ColumnSpec: a size and an alignment, no name and no type
//   an entity id                   ->  the key, whose size the layout is told and whose meaning it
//                                      is not
//   a component's change version   ->  a column's version
//
// A reader who wants to know what an entity is should be reading `ecs-core`, and a reader who wants
// to know how 16 KiB is divided into aligned columns should be reading this.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/chunk_allocator.h>

#include <cstring>

namespace cy {

/// The most columns one layout may have. Fixed so a layout is a value that can be copied and stored
/// beside the thing it describes, rather than an object with its own allocation.
inline constexpr u32 kMaxChunkColumns = 64;

/// One column's storage requirement. No name, no type, no identity — see the note above.
struct ColumnSpec {
    u32 size = 0;
    u32 alignment = 1;
};

/// How one chunk is divided. Computed once from a column set and then read on every access.
class ChunkLayout {
public:
    ChunkLayout() noexcept = default;

    /// Solve for the largest row count whose header, versions, key array and columns fit
    /// `chunk_bytes`.
    ///
    /// The search is a binary search over the row count with an exact cost function, rather than a
    /// closed form: alignment padding between columns depends on the row count, so a division by
    /// the summed row size is an estimate and this is the answer.
    [[nodiscard]] static Expected<ChunkLayout, Error> compute(
        u32 chunk_bytes, u32 key_size, u32 key_alignment, Span<const ColumnSpec> columns) noexcept;

    [[nodiscard]] u32 chunk_bytes() const noexcept { return chunk_bytes_; }
    /// The rows one chunk holds. Recorded here, which is where the archetype above reads it from.
    [[nodiscard]] u32 capacity() const noexcept { return capacity_; }
    [[nodiscard]] u32 column_count() const noexcept { return column_count_; }
    [[nodiscard]] u32 key_size() const noexcept { return key_size_; }
    [[nodiscard]] u32 key_offset() const noexcept { return key_offset_; }
    [[nodiscard]] u32 column_offset(u32 column) const noexcept { return offsets_[column]; }
    [[nodiscard]] u32 column_size(u32 column) const noexcept { return columns_[column].size; }
    /// Where a chunk's per-column versions live, relative to the front of the chunk.
    [[nodiscard]] u32 versions_offset() const noexcept { return versions_offset_; }
    /// Bytes a chunk of this layout actually uses, which is at most `chunk_bytes()`.
    [[nodiscard]] u32 used_bytes() const noexcept { return used_bytes_; }

private:
    u32 chunk_bytes_ = 0;
    u32 capacity_ = 0;
    u32 column_count_ = 0;
    u32 key_size_ = 0;
    u32 key_offset_ = 0;
    u32 versions_offset_ = 0;
    u32 used_bytes_ = 0;
    ColumnSpec columns_[kMaxChunkColumns] = {};
    u32 offsets_[kMaxChunkColumns] = {};
};

/// The fixed part at the front of every chunk. Everything else is derived from the layout.
struct ChunkHeader {
    u32 count = 0;
    u32 capacity = 0;
    u32 column_count = 0;
    u32 reserved = 0;
};

/// A view over one chunk of memory laid out by a `ChunkLayout`.
///
/// It is a view and not an owner: the memory comes from a `ChunkAllocator`, and `ChunkStore` below
/// is what owns the pairing. Constructing one over a block that was not laid out by the same
/// layout is a programmer error with no diagnostic, which is why the constructor is not public and
/// `ChunkStore` is the only thing that calls it.
class ChunkView {
public:
    ChunkView(void* memory, const ChunkLayout& layout) noexcept
        : memory_(static_cast<u8*>(memory)), layout_(&layout) {}

    [[nodiscard]] u32 count() const noexcept { return header().count; }
    [[nodiscard]] u32 capacity() const noexcept { return layout_->capacity(); }
    [[nodiscard]] bool full() const noexcept { return count() == capacity(); }
    [[nodiscard]] bool empty() const noexcept { return count() == 0; }

    /// The key array. First after the header and the versions, as the specification requires.
    [[nodiscard]] void* keys() noexcept { return memory_ + layout_->key_offset(); }
    [[nodiscard]] const void* keys() const noexcept { return memory_ + layout_->key_offset(); }

    template <class K>
    [[nodiscard]] Span<K> keys_as() noexcept {
        CY_ASSERT_MSG(sizeof(K) == layout_->key_size(), "key type does not match the layout");
        return Span<K>(static_cast<K*>(keys()), count());
    }

    [[nodiscard]] void* column(u32 index) noexcept {
        return memory_ + layout_->column_offset(index);
    }
    [[nodiscard]] const void* column(u32 index) const noexcept {
        return memory_ + layout_->column_offset(index);
    }

    /// A column as a typed, contiguous span of the live rows. THE ITERATION PATH: one span, walked
    /// forward, with no per-row indirection and no lookup.
    template <class T>
    [[nodiscard]] Span<T> column_as(u32 index) noexcept {
        CY_ASSERT_MSG(sizeof(T) == layout_->column_size(index),
                      "column type does not match the layout");
        return Span<T>(static_cast<T*>(column(index)), count());
    }

    /// Append a row and return its index, or nothing when the chunk is full. The row's bytes are
    /// NOT initialised: the caller places what belongs there, which is what keeps this file free of
    /// any knowledge of what a column holds.
    [[nodiscard]] Expected<u32, Error> add_row() noexcept {
        ChunkHeader& head = header();
        if (head.count >= head.capacity) {
            return fail(ErrorCode::OutOfRange, "chunk is full");
        }
        return head.count++;
    }

    /// Remove a row by moving the last one into its place. O(1) per column, and the caller is
    /// responsible for having destroyed whatever was in `row` first.
    void remove_row_unordered(u32 row) noexcept {
        ChunkHeader& head = header();
        CY_ASSERT_MSG(row < head.count, "remove_row_unordered() past the end");
        if (row >= head.count) {
            return;
        }
        const u32 last = --head.count;
        if (row == last) {
            return;
        }
        move_row_bytes(row, last);
    }

    /// The version stamped on `column` when it was last written.
    [[nodiscard]] u64 version(u32 column) const noexcept { return versions()[column]; }
    void set_version(u32 column, u64 value) noexcept { versions()[column] = value; }

    /// Stamp every column. What a bulk write — a chunk being filled, or moved — does once instead
    /// of per row.
    void set_all_versions(u64 value) noexcept {
        for (u32 index = 0; index < layout_->column_count(); ++index) {
            versions()[index] = value;
        }
    }

    /// Whether `column` changed since `since`. O(1), which is the whole requirement.
    [[nodiscard]] bool changed_since(u32 column, u64 since) const noexcept {
        return version(column) > since;
    }

    [[nodiscard]] const ChunkLayout& layout() const noexcept { return *layout_; }
    [[nodiscard]] void* memory() noexcept { return memory_; }

private:
    [[nodiscard]] ChunkHeader& header() noexcept {
        return *static_cast<ChunkHeader*>(static_cast<void*>(memory_));
    }
    [[nodiscard]] const ChunkHeader& header() const noexcept {
        return *static_cast<const ChunkHeader*>(static_cast<const void*>(memory_));
    }
    [[nodiscard]] u64* versions() noexcept {
        return static_cast<u64*>(static_cast<void*>(memory_ + layout_->versions_offset()));
    }
    [[nodiscard]] const u64* versions() const noexcept {
        return static_cast<const u64*>(
            static_cast<const void*>(memory_ + layout_->versions_offset()));
    }

    void move_row_bytes(u32 target, u32 source) noexcept;

    u8* memory_;
    const ChunkLayout* layout_;
};

/// Chunks of one layout, and the rows spread across them.
///
/// The store hands out chunks and tracks which of them have room; it does not know what a row means
/// and does not iterate rows itself. A consumer iterates chunks and, within each, columns.
class ChunkStore {
public:
    ChunkStore(const ChunkLayout& layout, MemoryDomain domain, AllocationTag tag) noexcept;
    ~ChunkStore();

    ChunkStore(const ChunkStore&) = delete;
    ChunkStore& operator=(const ChunkStore&) = delete;

    /// Where the next row goes: the chunk and the row index within it. Allocates a chunk when every
    /// existing one is full.
    struct RowLocation {
        u32 chunk = 0;
        u32 row = 0;
    };

    [[nodiscard]] Expected<RowLocation, Error> add_row() noexcept;

    /// Remove a row, moving the last row of its chunk into the gap. Returns where the moved row
    /// came from, so the caller above can fix up whatever index it keeps — which is the ECS's job
    /// and not this file's.
    struct RowMove {
        bool moved = false;
        u32 chunk = 0;
        u32 from_row = 0;
        u32 to_row = 0;
    };

    RowMove remove_row(u32 chunk, u32 row) noexcept;

    [[nodiscard]] u32 chunk_count() const noexcept { return static_cast<u32>(chunks_.size()); }
    [[nodiscard]] ChunkView chunk(u32 index) noexcept { return ChunkView(chunks_[index], layout_); }
    [[nodiscard]] u64 row_count() const noexcept;
    [[nodiscard]] const ChunkLayout& layout() const noexcept { return layout_; }

    /// Bump the store's version and stamp `column` in `chunk` with it. The version is the store's,
    /// so versions are comparable across every chunk it owns.
    u64 touch(u32 chunk, u32 column) noexcept;
    [[nodiscard]] u64 current_version() const noexcept { return version_; }

    /// Give every empty chunk back to the allocator's free list. The pressure response for storage
    /// that has shrunk.
    usize trim() noexcept;

    [[nodiscard]] u64 committed_bytes() const noexcept { return allocator_.committed_bytes(); }

private:
    ChunkLayout layout_;
    ChunkAllocator allocator_;
    Array<void*> chunks_;
    u64 version_ = 1;
};

}  // namespace cy
