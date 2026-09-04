// Archetypes over M1's chunked storage. Task 2.3.

#include <cy/ecs/archetype.h>

#include <cy/core/memory/domain.h>

#include <cstring>

namespace cy::ecs {
namespace {

/// Insertion sort into ascending order, rejecting a duplicate. The set is at most
/// `kMaxChunkColumns` long and is sorted once per archetype creation, so the simple algorithm is
/// the right one; what matters is that the result is canonical, because two entities given the same
/// components in different orders must reach the same archetype.
[[nodiscard]] Status sorted_unique(Span<const ComponentTypeId> input,
                                   Array<ComponentTypeId>& out) noexcept {
    for (const ComponentTypeId component : input) {
        usize position = out.size();
        while (position > 0 && out[position - 1] > component) {
            --position;
        }
        if (position > 0 && out[position - 1] == component) {
            return fail(ErrorCode::InvalidArgument, "a component set names one component twice");
        }
        if (Status pushed = out.push_back(component); !pushed) {
            return pushed;
        }
        for (usize index = out.size() - 1; index > position; --index) {
            const ComponentTypeId moved = out[index - 1];
            out[index - 1] = out[index];
            out[index] = moved;
        }
    }
    return ok();
}

}  // namespace

Archetype::Archetype(Allocator& allocator, u32 id) noexcept
    : allocator_(&allocator),
      id_(id),
      components_(allocator),
      columns_(allocator),
      releasing_columns_(allocator),
      releasers_(allocator),
      shared_(allocator) {}

Archetype::~Archetype() {
    if (store_ == nullptr) {
        return;
    }
    // Every live row's buffer components own a heap block. Releasing them here rather than relying
    // on the chunk allocator is the difference between the world's memory going back to M1's budget
    // tree and the spill blocks leaking under it.
    if (!releasing_columns_.empty()) {
        for (u32 index = 0; index < store_->chunk_count(); ++index) {
            ChunkView view = store_->chunk(index);
            for (u32 row = 0; row < view.count(); ++row) {
                release_buffers(index, row);
            }
        }
    }
    store_->~ChunkStore();
    allocator_->deallocate(static_cast<void*>(store_), sizeof(ChunkStore), alignof(ChunkStore));
    store_ = nullptr;
}

Expected<Archetype*, Error> Archetype::create(
    Allocator& allocator, u32 id, const ComponentMask& mask, Span<const ComponentTypeId> components,
    Span<const SharedValue> shared, const ComponentRegistry& registry, u32 chunk_bytes) noexcept {
    void* block = allocator.allocate(sizeof(Archetype), alignof(Archetype));
    if (block == nullptr) {
        return fail(ErrorCode::OutOfMemory, "could not allocate an archetype");
    }
    auto* archetype = ::new (block) Archetype(allocator, id);
    archetype->mask_ = mask;

    // A single failure path: anything below that fails destroys the half-built archetype rather
    // than returning one whose invariants are partly established.
    const auto abort = [&](Error error) -> Expected<Archetype*, Error> {
        Archetype::destroy(allocator, archetype);
        return make_unexpected(error);
    };

    if (Status sorted = sorted_unique(components, archetype->components_); !sorted) {
        return abort(sorted.error());
    }
    if (Status appended = archetype->shared_.append(shared); !appended) {
        return abort(appended.error());
    }

    FixedArray<ColumnSpec, kMaxChunkColumns> specs;
    for (const ComponentTypeId component : archetype->components_) {
        const ComponentInfo& info = registry.info(component);
        if (!kind_has_column(info.kind)) {
            continue;
        }
        if (Status pushed = archetype->columns_.push_back(component); !pushed) {
            return abort(pushed.error());
        }
        Expected<ColumnSpec*, Error> spec =
            specs.emplace_back(ColumnSpec{info.size, info.alignment});
        if (!spec) {
            return abort(spec.error());
        }
        if (info.release != nullptr) {
            const auto column = static_cast<u32>(archetype->columns_.size() - 1);
            if (Status pushed = archetype->releasing_columns_.push_back(column); !pushed) {
                return abort(pushed.error());
            }
            if (Status pushed = archetype->releasers_.push_back(info.release); !pushed) {
                return abort(pushed.error());
            }
        }
    }

    // The key is the Entity, and `chunk_storage.h` is told its size and alignment rather than its
    // meaning. That split is the whole reason the chunk allocator is M1's and this file is not.
    Expected<ChunkLayout, Error> layout = ChunkLayout::compute(
        chunk_bytes, static_cast<u32>(sizeof(Entity)), static_cast<u32>(alignof(Entity)),
        Span<const ColumnSpec>(specs.data(), specs.size()));
    if (!layout) {
        return abort(layout.error());
    }

    void* store_block = allocator.allocate(sizeof(ChunkStore), alignof(ChunkStore));
    if (store_block == nullptr) {
        return abort(Error{ErrorCode::OutOfMemory, "could not allocate a chunk store"});
    }
    archetype->store_ = ::new (store_block) ChunkStore(*layout, MemoryDomain::Ecs, "ecs-archetype");
    return archetype;
}

void Archetype::destroy(Allocator& allocator, Archetype* archetype) noexcept {
    if (archetype == nullptr) {
        return;
    }
    archetype->~Archetype();
    allocator.deallocate(static_cast<void*>(archetype), sizeof(Archetype), alignof(Archetype));
}

i32 Archetype::column_of(ComponentTypeId component) const noexcept {
    usize low = 0;
    usize high = columns_.size();
    while (low < high) {
        const usize middle = low + ((high - low) / 2);
        if (columns_[middle] < component) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return (low < columns_.size() && columns_[low] == component) ? static_cast<i32>(low) : -1;
}

void Archetype::clear_row(u32 chunk, u32 row) noexcept {
    ChunkView view = store_->chunk(chunk);
    const ChunkLayout& layout = view.layout();
    for (u32 column = 0; column < layout.column_count(); ++column) {
        const auto size = static_cast<usize>(layout.column_size(column));
        auto* base = static_cast<u8*>(view.column(column));
        std::memset(base + (size * row), 0, size);
    }
}

void Archetype::clear_rows(u64 version) noexcept {
    for (u32 index = 0; index < store_->chunk_count(); ++index) {
        ChunkView view = store_->chunk(index);
        while (view.count() != 0) {
            const u32 last = view.count() - 1;
            release_buffers(index, last);
            // Removing the last row moves nothing, so this is a count decrement per row and no
            // data movement at all.
            view.remove_row_unordered(last);
        }
        view.set_all_versions(version);
    }
}

void Archetype::stamp(u32 chunk, u64 version) noexcept {
    store_->chunk(chunk).set_all_versions(version);
}

Expected<ChunkStore::RowLocation, Error> Archetype::add_row(u64 version) noexcept {
    Expected<ChunkStore::RowLocation, Error> location = store_->add_row();
    if (!location) {
        return location;
    }
    clear_row(location->chunk, location->row);
    // A row added changes every column of its chunk: change detection is chunk-granular by
    // specification, so a change filter must see the chunk whatever component it watches.
    stamp(location->chunk, version);
    return location;
}

Status Archetype::add_rows(u32 count, u64 version, Array<RowRange>& out) noexcept {
    u32 remaining = count;
    while (remaining != 0) {
        // One store call per chunk. It is what finds or acquires a chunk with room; the rest of the
        // run is `ChunkView::add_row`, which is a bounds check and an increment.
        Expected<ChunkStore::RowLocation, Error> first = store_->add_row();
        if (!first) {
            return make_unexpected(first.error());
        }
        ChunkView view = store_->chunk(first->chunk);
        u32 taken = 1;
        while (taken < remaining && !view.full()) {
            Expected<u32, Error> row = view.add_row();
            if (!row) {
                break;
            }
            ++taken;
        }

        const ChunkLayout& layout = view.layout();
        for (u32 column = 0; column < layout.column_count(); ++column) {
            const auto size = static_cast<usize>(layout.column_size(column));
            auto* base = static_cast<u8*>(view.column(column));
            std::memset(base + (size * first->row), 0, size * taken);
        }
        view.set_all_versions(version);

        if (Status pushed = out.push_back(RowRange{first->chunk, first->row, taken}); !pushed) {
            return pushed;
        }
        remaining -= taken;
    }
    return ok();
}

void Archetype::release_buffers(u32 chunk, u32 row) noexcept {
    release_buffers_except(chunk, row, ComponentMask{});
}

void Archetype::release_buffers_except(u32 chunk, u32 row, const ComponentMask& keep) noexcept {
    if (releasing_columns_.empty()) {
        return;
    }
    ChunkView view = store_->chunk(chunk);
    const ChunkLayout& layout = view.layout();
    for (usize index = 0; index < releasing_columns_.size(); ++index) {
        const u32 column = releasing_columns_[index];
        if (keep.test(columns_[column])) {
            continue;
        }
        const auto size = static_cast<usize>(layout.column_size(column));
        auto* base = static_cast<u8*>(view.column(column));
        releasers_[index](static_cast<void*>(base + (size * row)), *allocator_);
    }
}

ChunkStore::RowMove Archetype::remove_row(u32 chunk, u32 row) noexcept {
    release_buffers(chunk, row);
    return store_->remove_row(chunk, row);
}

void* Archetype::value_at(u32 chunk, u32 row, ComponentTypeId component) noexcept {
    const i32 column = column_of(component);
    if (column < 0) {
        return nullptr;
    }
    ChunkView view = store_->chunk(chunk);
    const auto size = static_cast<usize>(view.layout().column_size(static_cast<u32>(column)));
    auto* base = static_cast<u8*>(view.column(static_cast<u32>(column)));
    return static_cast<void*>(base + (size * row));
}

void Archetype::copy_shared_columns(Archetype& source, u32 source_chunk, u32 source_row,
                                    u32 target_chunk, u32 target_row) noexcept {
    ChunkView from = source.store_->chunk(source_chunk);
    ChunkView to = store_->chunk(target_chunk);
    for (usize index = 0; index < columns_.size(); ++index) {
        const i32 source_column = source.column_of(columns_[index]);
        if (source_column < 0) {
            continue;
        }
        const auto size = static_cast<usize>(to.layout().column_size(static_cast<u32>(index)));
        auto* target_base = static_cast<u8*>(to.column(static_cast<u32>(index)));
        auto* source_base = static_cast<u8*>(from.column(static_cast<u32>(source_column)));
        std::memcpy(static_cast<void*>(target_base + (size * target_row)),
                    static_cast<const void*>(source_base + (size * source_row)), size);
    }
}

// --- The table ------------------------------------------------------------------------------

ArchetypeTable::~ArchetypeTable() {
    clear();
}

void ArchetypeTable::clear() noexcept {
    for (Archetype* archetype : archetypes_) {
        Archetype::destroy(*allocator_, archetype);
    }
    archetypes_.clear();
}

Expected<Archetype*, Error> ArchetypeTable::find_or_create(const ComponentMask& mask,
                                                           Span<const ComponentTypeId> components,
                                                           Span<const SharedValue> shared,
                                                           const ComponentRegistry& registry) {
    for (Archetype* candidate : archetypes_) {
        if (!(candidate->mask() == mask)) {
            continue;
        }
        // The mask says which shared components are present; the values say which chunks they group
        // into. Two archetypes with the same mask and different shared values are different
        // archetypes, which is what "entities sharing it group into the same chunk" means.
        const Span<const SharedValue> existing = candidate->shared();
        if (existing.size() != shared.size()) {
            continue;
        }
        bool same = true;
        for (usize index = 0; index < shared.size(); ++index) {
            if (!(existing[index] == shared[index])) {
                same = false;
                break;
            }
        }
        if (same) {
            return candidate;
        }
    }

    const auto id = static_cast<u32>(archetypes_.size());
    Expected<Archetype*, Error> created =
        Archetype::create(*allocator_, id, mask, components, shared, registry, chunk_bytes_);
    if (!created) {
        return created;
    }
    if (Status pushed = archetypes_.push_back(*created); !pushed) {
        Archetype::destroy(*allocator_, *created);
        return make_unexpected(pushed.error());
    }
    return *created;
}

usize ArchetypeTable::trim() noexcept {
    usize released = 0;
    for (Archetype* archetype : archetypes_) {
        released += archetype->store().trim();
    }
    return released;
}

}  // namespace cy::ecs
