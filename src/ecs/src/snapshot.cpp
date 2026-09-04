// Snapshots and the world byte stream. Task 2.10.

#include <cy/ecs/snapshot.h>

#include <cstring>

namespace cy::ecs {
namespace {

/// Rows in an archetype's chunks, walked in (chunk, row) order. That order is the snapshot's and
/// the stream's, and it is stable because chunks are appended and rows are compacted from the end.
[[nodiscard]] u32 total_rows(Archetype& archetype) noexcept {
    u32 rows = 0;
    for (u32 index = 0; index < archetype.chunk_count(); ++index) {
        rows += archetype.chunk(index).count();
    }
    return rows;
}

// NOLINTBEGIN(bugprone-casting-through-void) — through void* on purpose. A chunk column is a byte
// block whose entries begin at offsets the layout aligned; a direct reinterpret_cast from `u8*` is
// what -Wcast-align reports, and the engine builds with -Werror. The alignment is established by
// ChunkLayout, which is where it belongs.
[[nodiscard]] BufferHeader* buffer_header_at(ChunkView& view, u32 column, u32 row) noexcept {
    const auto size = static_cast<usize>(view.layout().column_size(column));
    auto* base = static_cast<u8*>(view.column(column));
    return static_cast<BufferHeader*>(static_cast<void*>(base + (size * row)));
}
// NOLINTEND(bugprone-casting-through-void)

/// Point a buffer header at a heap block holding `count` elements copied from `source`, or at
/// nothing when the elements fit inline. The untyped half of buffer.h: a snapshot restores a buffer
/// without knowing its element type, so the size and alignment come from the registry instead.
[[nodiscard]] Status restore_buffer(BufferHeader& header, const ComponentInfo& info,
                                    const u8* source, u32 count, Allocator& allocator) noexcept {
    header.heap = nullptr;
    header.heap_capacity = 0;
    if (count == 0) {
        return ok();
    }
    void* block = allocator.allocate(usize{count} * info.element_size, info.element_alignment);
    if (block == nullptr) {
        return fail(ErrorCode::OutOfMemory, "could not restore a buffer component's spill");
    }
    std::memcpy(block, static_cast<const void*>(source), usize{count} * info.element_size);
    header.heap = block;
    header.heap_capacity = count;
    return ok();
}

}  // namespace

// --- Snapshot -------------------------------------------------------------------------------

Snapshot::Snapshot(Allocator& allocator) noexcept
    : allocator_(&allocator),
      archetypes_(allocator),
      sparse_(allocator),
      shared_(allocator),
      records_(allocator),
      free_indices_(allocator),
      resources_(allocator) {}

Snapshot::~Snapshot() = default;

u64 Snapshot::bytes() const noexcept {
    u64 total = records_.size() * sizeof(EntityTable::Record);
    for (const ArchetypeCopy& copy : archetypes_) {
        total +=
            copy.columns.size() + copy.buffer_data.size() + (copy.keys.size() * sizeof(Entity));
    }
    for (const SparseCopy& copy : sparse_) {
        total += copy.values.size() + (copy.keys.size() * sizeof(u32) * 2);
    }
    for (const SharedCopy& copy : shared_) {
        total += copy.bytes.size();
    }
    return total;
}

Status Snapshot::capture_spills(ChunkView& view, const ComponentInfo& info, u32 column,
                                ArchetypeCopy& copy) noexcept {
    // A buffer's heap block is deep-copied. Copying the pointer would give the snapshot and the
    // world one block with two owners, and the first restore would free it twice.
    for (u32 row = 0; row < view.count(); ++row) {
        const BufferHeader* header = buffer_header_at(view, column, row);
        const u32 spilled = (header->heap != nullptr) ? header->size : 0;
        if (Status pushed = copy.buffer_sizes.push_back(spilled); !pushed) {
            return pushed;
        }
        if (spilled == 0) {
            continue;
        }
        const auto* elements = static_cast<const u8*>(header->heap);
        if (Status appended = copy.buffer_data.append(
                Span<const u8>(elements, usize{spilled} * info.element_size));
            !appended) {
            return appended;
        }
    }
    return ok();
}

Status Snapshot::capture_column(Archetype& archetype, const ComponentInfo& info, u32 column,
                                ArchetypeCopy& copy) noexcept {
    if (Status pushed = copy.column_offsets.push_back(copy.columns.size()); !pushed) {
        return pushed;
    }
    for (u32 chunk = 0; chunk < archetype.chunk_count(); ++chunk) {
        ChunkView view = archetype.chunk(chunk);
        const auto size = static_cast<usize>(view.layout().column_size(column));
        const auto* base = static_cast<const u8*>(view.column(column));
        if (Status appended = copy.columns.append(Span<const u8>(base, size * view.count()));
            !appended) {
            return appended;
        }
        if (info.kind != ComponentKind::Buffer) {
            continue;
        }
        if (Status captured = capture_spills(view, info, column, copy); !captured) {
            return captured;
        }
    }
    return ok();
}

Status Snapshot::capture_archetype(const ComponentRegistry& registry, Archetype& archetype,
                                   ArchetypeCopy& copy) noexcept {
    copy.rows = total_rows(archetype);
    if (Status appended = copy.components.append(archetype.components()); !appended) {
        return appended;
    }
    if (Status appended = copy.shared.append(archetype.shared()); !appended) {
        return appended;
    }

    // Column-major: every row of one column, then the next. That is the order a restore refills in,
    // so the copy is a memcpy per column per chunk rather than a walk per row.
    u32 column = 0;
    for (const ComponentTypeId component : archetype.components()) {
        const ComponentInfo& info = registry.info(component);
        if (!kind_has_column(info.kind)) {
            continue;
        }
        if (Status captured = capture_column(archetype, info, column, copy); !captured) {
            return captured;
        }
        ++column;
    }

    for (u32 chunk = 0; chunk < archetype.chunk_count(); ++chunk) {
        ChunkView view = archetype.chunk(chunk);
        const auto* keys = static_cast<const Entity*>(view.keys());
        if (Status appended = copy.keys.append(Span<const Entity>(keys, view.count())); !appended) {
            return appended;
        }
    }
    return ok();
}

Status Snapshot::capture(World& world) noexcept {
    if (world.iterating()) {
        return fail(ErrorCode::Unavailable,
                    "a snapshot of a world mid-iteration is a snapshot of a world about to change");
    }
    archetypes_.clear();
    sparse_.clear();
    shared_.clear();
    entity_count_ = 0;
    version_ = world.version();

    for (u32 index = 0; index < world.archetypes().size(); ++index) {
        ArchetypeCopy copy(*allocator_);
        if (Status captured =
                capture_archetype(world.components(), world.archetypes().at(index), copy);
            !captured) {
            return captured;
        }
        entity_count_ += copy.rows;
        if (Status pushed = archetypes_.push_back(std::move(copy)); !pushed) {
            return pushed;
        }
    }

    if (Status captured = capture_side_tables(world); !captured) {
        return captured;
    }

    records_.clear();
    free_indices_.clear();
    if (Status appended = records_.append(world.entities().records()); !appended) {
        return appended;
    }
    if (Status appended = free_indices_.append(world.entities().free_indices()); !appended) {
        return appended;
    }
    live_ = world.entities().live_count();
    return ok();
}

Status Snapshot::restore(World& world) const noexcept {
    if (world.iterating()) {
        return fail(ErrorCode::Unavailable, "a restore is a structural change");
    }
    const ComponentRegistry& registry = world.components();

    // Every archetype is emptied, including ones created after the capture: they are part of the
    // world's state and a restore is a replacement, not a merge. The archetypes themselves stay,
    // so every cached query keeps its indices.
    for (u32 index = 0; index < world.archetypes().size(); ++index) {
        world.archetypes().at(index).clear_rows(version_);
    }

    for (const ArchetypeCopy& copy : archetypes_) {
        if (copy.rows == 0) {
            continue;
        }
        ComponentMask mask;
        for (const ComponentTypeId component : copy.components) {
            if (kind_changes_archetype(registry.info(component).kind)) {
                mask.set(component);
            }
        }
        Expected<Archetype*, Error> archetype = world.archetypes().find_or_create(
            mask, copy.components.span(), copy.shared.span(), registry);
        if (!archetype) {
            return make_unexpected(archetype.error());
        }

        Array<Archetype::RowRange> ranges(*allocator_);
        if (Status added = (*archetype)->add_rows(copy.rows, version_, ranges); !added) {
            return added;
        }
        if (Status written = write_rows(world, **archetype, copy, ranges); !written) {
            return written;
        }
    }

    if (Status restored = restore_side_tables(world); !restored) {
        return restored;
    }
    if (Status restored = world.entities_.restore(records_.span(), free_indices_.span(), live_);
        !restored) {
        return restored;
    }
    // Placement is re-derived rather than assumed: a cleared chunk store hands out rows in its own
    // order, which is not necessarily the order the capture saw. The keys say who is where.
    return reindex(world);
}

Status Snapshot::capture_sparse(World& world) noexcept {
    for (const World::SparseSlot& slot : world.sparse_) {
        SparseCopy copy(*allocator_);
        copy.component = slot.component;
        if (Status appended = copy.keys.append(slot.store.keys()); !appended) {
            return appended;
        }
        if (Status appended = copy.generations.append(slot.store.generations()); !appended) {
            return appended;
        }
        if (Status appended = copy.values.append(slot.store.values()); !appended) {
            return appended;
        }
        if (Status pushed = sparse_.push_back(std::move(copy)); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status Snapshot::capture_shared(World& world) noexcept {
    for (usize index = 0; index < world.shared_tables_.size(); ++index) {
        const World::SharedTable& table = world.shared_tables_[index];
        SharedCopy copy(*allocator_);
        copy.component = static_cast<ComponentTypeId>(index);
        copy.value_size = table.value_size;
        copy.count = table.count;
        if (Status appended = copy.bytes.append(table.bytes.span()); !appended) {
            return appended;
        }
        if (Status pushed = shared_.push_back(std::move(copy)); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status Snapshot::capture_resources(World& world) noexcept {
    resources_.clear();
    const ResourceRegistry& resources = world.resources();
    for (ResourceId id = 0; id < resources.size(); ++id) {
        const auto* value = static_cast<const u8*>(resources.get(id));
        if (value == nullptr) {
            continue;
        }
        if (Status appended = resources_.append(Span<const u8>(value, resources.value_size(id)));
            !appended) {
            return appended;
        }
    }
    return ok();
}

Status Snapshot::capture_side_tables(World& world) noexcept {
    if (Status captured = capture_sparse(world); !captured) {
        return captured;
    }
    if (Status captured = capture_shared(world); !captured) {
        return captured;
    }
    return capture_resources(world);
}

Status Snapshot::restore_sparse(World& world) const noexcept {
    for (World::SparseSlot& slot : world.sparse_) {
        slot.store.clear();
    }
    for (const SparseCopy& copy : sparse_) {
        if (Status ensured = world.ensure_sparse_store(copy.component); !ensured) {
            return ensured;
        }
        SparseStore* store = world.sparse_store(copy.component);
        const u32 value_size = store->value_size();
        for (usize index = 0; index < copy.keys.size(); ++index) {
            const Entity entity = Entity::make(copy.keys[index], copy.generations[index]);
            const u8* value =
                (value_size == 0) ? nullptr : (copy.values.data() + (index * value_size));
            if (Status set = store->set(entity, static_cast<const void*>(value)); !set) {
                return set;
            }
        }
    }
    return ok();
}

Status Snapshot::restore_shared(World& world) const noexcept {
    for (const SharedCopy& copy : shared_) {
        while (world.shared_tables_.size() <= copy.component) {
            World::SharedTable table(world.allocator());
            if (Status pushed = world.shared_tables_.push_back(std::move(table)); !pushed) {
                return pushed;
            }
        }
        World::SharedTable& table = world.shared_tables_[copy.component];
        table.value_size = copy.value_size;
        table.count = copy.count;
        table.bytes.clear();
        if (Status appended = table.bytes.append(copy.bytes.span()); !appended) {
            return appended;
        }
    }
    return ok();
}

void Snapshot::restore_resources(World& world) const noexcept {
    ResourceRegistry& resources = world.resources();
    usize offset = 0;
    for (ResourceId id = 0; id < resources.size() && offset < resources_.size(); ++id) {
        auto* value = static_cast<u8*>(resources.get(id));
        if (value == nullptr) {
            continue;
        }
        const u32 size = resources.value_size(id);
        if (offset + size > resources_.size()) {
            return;
        }
        std::memcpy(static_cast<void*>(value), static_cast<const void*>(resources_.data() + offset),
                    size);
        offset += size;
    }
}

Status Snapshot::restore_side_tables(World& world) const noexcept {
    if (Status restored = restore_sparse(world); !restored) {
        return restored;
    }
    if (Status restored = restore_shared(world); !restored) {
        return restored;
    }
    restore_resources(world);
    world.version_ = version_;
    return ok();
}

Status Snapshot::write_rows(World& world, Archetype& archetype, const ArchetypeCopy& copy,
                            Span<const Archetype::RowRange> ranges) noexcept {
    const ComponentRegistry& registry = world.components();
    u32 column = 0;
    usize buffer_cursor = 0;
    usize buffer_entry = 0;
    for (const ComponentTypeId component : copy.components) {
        const ComponentInfo& info = registry.info(component);
        if (!kind_has_column(info.kind)) {
            continue;
        }
        const usize source_base = copy.column_offsets[column];
        u32 written = 0;
        for (const Archetype::RowRange& range : ranges) {
            ChunkView view = archetype.chunk(range.chunk);
            const auto size = static_cast<usize>(view.layout().column_size(column));
            auto* target = static_cast<u8*>(view.column(column));
            std::memcpy(
                static_cast<void*>(target + (size * range.first_row)),
                static_cast<const void*>(copy.columns.data() + source_base + (size * written)),
                size * range.count);
            written += range.count;
        }

        if (info.kind == ComponentKind::Buffer) {
            // The headers just copied still name the *captured* world's heap blocks. Each one is
            // repointed at a fresh copy of the elements the snapshot holds, or at nothing.
            u32 row_index = 0;
            for (const Archetype::RowRange& range : ranges) {
                ChunkView view = archetype.chunk(range.chunk);
                for (u32 offset = 0; offset < range.count; ++offset) {
                    BufferHeader* header = buffer_header_at(view, column, range.first_row + offset);
                    const u32 spilled = copy.buffer_sizes[buffer_entry + row_index];
                    if (Status restored =
                            restore_buffer(*header, info, copy.buffer_data.data() + buffer_cursor,
                                           spilled, world.allocator());
                        !restored) {
                        return restored;
                    }
                    buffer_cursor += usize{spilled} * info.element_size;
                    ++row_index;
                }
            }
            buffer_entry += row_index;
        }
        ++column;
    }

    u32 key_index = 0;
    for (const Archetype::RowRange& range : ranges) {
        ChunkView view = archetype.chunk(range.chunk);
        auto* keys = static_cast<Entity*>(view.keys());
        for (u32 offset = 0; offset < range.count; ++offset) {
            keys[range.first_row + offset] = copy.keys[key_index++];
        }
    }
    return ok();
}

Status Snapshot::reindex(World& world) noexcept {
    for (u32 index = 0; index < world.archetypes().size(); ++index) {
        Archetype& archetype = world.archetypes().at(index);
        for (u32 chunk = 0; chunk < archetype.chunk_count(); ++chunk) {
            ChunkView view = archetype.chunk(chunk);
            const auto* keys = static_cast<const Entity*>(view.keys());
            for (u32 row = 0; row < view.count(); ++row) {
                world.entities_.set_location(keys[row], EntityLocation{index, chunk, row});
            }
        }
    }
    return ok();
}

}  // namespace cy::ecs
