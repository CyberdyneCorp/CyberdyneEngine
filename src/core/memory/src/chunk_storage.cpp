// Chunk layout solving, and the store that owns chunks of one layout. Task 2.6.

#include <cy/core/memory/chunk_storage.h>

#include <cy/core/memory/system_allocator.h>

namespace cy {
namespace {

/// The bytes a chunk of `columns` needs to hold `rows`, and where each part lands.
///
/// The order is fixed by the specification: header, then the versions, then the key array, then the
/// columns — "with the entity id array first and each component array aligned to its type's
/// alignment", read one level down as "the key array first".
struct LayoutSolution {
    u32 total = 0;
    u32 versions_offset = 0;
    u32 key_offset = 0;
    u32 offsets[kMaxChunkColumns] = {};
};

[[nodiscard]] constexpr u32 round_to(u32 value, u32 alignment) noexcept {
    return (alignment <= 1) ? value : ((value + alignment - 1) / alignment) * alignment;
}

[[nodiscard]] LayoutSolution solve(u32 rows, u32 key_size, u32 key_alignment,
                                   Span<const ColumnSpec> columns) noexcept {
    LayoutSolution solution;
    u32 offset = static_cast<u32>(sizeof(ChunkHeader));

    solution.versions_offset = round_to(offset, static_cast<u32>(alignof(u64)));
    offset = solution.versions_offset + static_cast<u32>(columns.size() * sizeof(u64));

    solution.key_offset = round_to(offset, key_alignment);
    offset = solution.key_offset + key_size * rows;

    for (usize index = 0; index < columns.size(); ++index) {
        offset = round_to(offset, columns[index].alignment);
        solution.offsets[index] = offset;
        offset += columns[index].size * rows;
    }
    solution.total = offset;
    return solution;
}

}  // namespace

Expected<ChunkLayout, Error> ChunkLayout::compute(u32 chunk_bytes, u32 key_size, u32 key_alignment,
                                                  Span<const ColumnSpec> columns) noexcept {
    if (columns.size() > kMaxChunkColumns) {
        return fail(ErrorCode::InvalidArgument, "a chunk layout holds at most kMaxChunkColumns");
    }
    if (key_size == 0 || !is_power_of_two(key_alignment)) {
        return fail(ErrorCode::InvalidArgument,
                    "a chunk layout needs a non-empty key with a power-of-two alignment");
    }
    for (const ColumnSpec& column : columns) {
        if (column.size == 0 || !is_power_of_two(column.alignment)) {
            return fail(ErrorCode::InvalidArgument,
                        "every column needs a non-zero size and a power-of-two alignment");
        }
    }

    // A chunk that cannot hold one row is a configuration error, not an empty chunk: the caller
    // has asked for a row larger than the chunk size, and silently producing a capacity of zero
    // would turn that into an infinite loop somewhere above.
    if (solve(1, key_size, key_alignment, columns).total > chunk_bytes) {
        return fail(
            ErrorCode::InvalidArgument,
            "one row does not fit the chunk size; raise the chunk size or split the columns");
    }

    // Binary search for the largest row count that fits. `high` starts one past an upper bound that
    // ignores alignment padding, which can only over-estimate.
    u32 row_bytes = key_size;
    for (const ColumnSpec& column : columns) {
        row_bytes += column.size;
    }
    u32 low = 1;
    u32 high = (chunk_bytes / ((row_bytes == 0) ? 1 : row_bytes)) + 1;
    while (low < high) {
        const u32 middle = low + (high - low + 1) / 2;
        if (solve(middle, key_size, key_alignment, columns).total <= chunk_bytes) {
            low = middle;
        } else {
            high = middle - 1;
        }
    }

    const LayoutSolution solution = solve(low, key_size, key_alignment, columns);

    ChunkLayout layout;
    layout.chunk_bytes_ = chunk_bytes;
    layout.capacity_ = low;
    layout.column_count_ = static_cast<u32>(columns.size());
    layout.key_size_ = key_size;
    layout.key_offset_ = solution.key_offset;
    layout.versions_offset_ = solution.versions_offset;
    layout.used_bytes_ = solution.total;
    for (usize index = 0; index < columns.size(); ++index) {
        layout.columns_[index] = columns[index];
        layout.offsets_[index] = solution.offsets[index];
    }
    return layout;
}

void ChunkView::move_row_bytes(u32 target, u32 source) noexcept {
    const u32 key_size = layout_->key_size();
    std::memcpy(memory_ + layout_->key_offset() + (key_size * target),
                memory_ + layout_->key_offset() + (key_size * source), key_size);
    for (u32 column = 0; column < layout_->column_count(); ++column) {
        const u32 size = layout_->column_size(column);
        const u32 base = layout_->column_offset(column);
        std::memcpy(memory_ + base + (size * target), memory_ + base + (size * source), size);
    }
}

ChunkStore::ChunkStore(const ChunkLayout& layout, MemoryDomain domain, AllocationTag tag) noexcept
    : layout_(layout),
      allocator_(domain, tag, layout.chunk_bytes(), 64),
      chunks_(system_allocator(domain)) {}

ChunkStore::~ChunkStore() {
    for (void* chunk : chunks_) {
        allocator_.release(chunk);
    }
    chunks_.clear();
}

Expected<ChunkStore::RowLocation, Error> ChunkStore::add_row() noexcept {
    // Newest chunk first: a store that is filling has room in the one it just took, and a store
    // that has had rows removed has room in an older one. Scanning from the end finds the first
    // case in one step and the second in a linear pass, which is what the ECS above will replace
    // with its own free-chunk list when that becomes the cost that matters.
    for (usize index = chunks_.size(); index > 0; --index) {
        ChunkView view(chunks_[index - 1], layout_);
        if (!view.full()) {
            Expected<u32, Error> row = view.add_row();
            if (!row) {
                return make_unexpected(row.error());
            }
            return RowLocation{static_cast<u32>(index - 1), *row};
        }
    }

    void* memory = allocator_.acquire();
    if (memory == nullptr) {
        return fail(ErrorCode::OutOfMemory, "chunk store could not acquire a chunk");
    }
    // The header and the versions are the only part of a chunk this file initialises; the rows are
    // the caller's to fill.
    auto* header = static_cast<ChunkHeader*>(memory);
    header->count = 0;
    header->capacity = layout_.capacity();
    header->column_count = layout_.column_count();
    header->reserved = 0;

    ChunkView view(memory, layout_);
    view.set_all_versions(version_);

    if (Status pushed = chunks_.push_back(memory); !pushed) {
        allocator_.release(memory);
        return make_unexpected(pushed.error());
    }
    Expected<u32, Error> row = view.add_row();
    if (!row) {
        return make_unexpected(row.error());
    }
    return RowLocation{static_cast<u32>(chunks_.size() - 1), *row};
}

ChunkStore::RowMove ChunkStore::remove_row(u32 chunk_index, u32 row) noexcept {
    RowMove move;
    if (chunk_index >= chunks_.size()) {
        return move;
    }
    ChunkView view(chunks_[chunk_index], layout_);
    const u32 last = view.count() - 1;
    if (row != last) {
        move.moved = true;
        move.chunk = chunk_index;
        move.from_row = last;
        move.to_row = row;
    }
    view.remove_row_unordered(row);
    return move;
}

u64 ChunkStore::row_count() const noexcept {
    u64 total = 0;
    for (void* chunk : chunks_) {
        total += ChunkView(chunk, layout_).count();
    }
    return total;
}

u64 ChunkStore::touch(u32 chunk_index, u32 column) noexcept {
    ++version_;
    if (chunk_index < chunks_.size()) {
        ChunkView(chunks_[chunk_index], layout_).set_version(column, version_);
    }
    return version_;
}

usize ChunkStore::trim() noexcept {
    usize released = 0;
    // Walk from the end so that removing an empty chunk does not disturb the indices of the ones
    // still to be examined. Callers holding a chunk index across a trim is exactly the hazard, and
    // it is why this returns a count rather than being called implicitly.
    for (usize index = chunks_.size(); index > 0; --index) {
        void* chunk = chunks_[index - 1];
        if (ChunkView(chunk, layout_).count() != 0) {
            continue;
        }
        allocator_.release(chunk);
        chunks_.erase(index - 1);
        ++released;
    }
    return released;
}

}  // namespace cy
