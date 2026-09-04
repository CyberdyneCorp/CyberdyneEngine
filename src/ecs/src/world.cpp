// The world: entities, archetype transitions, and the storage they live in. Tasks 2.1-2.3, 2.6-2.8.

#include <cy/ecs/world.h>

#include <cy/ecs/command_buffer.h>

#include <cstring>

namespace cy::ecs {
namespace {

/// Write an entity into a chunk's key column. The key is the `Entity` the layout was told the size
/// of and not the meaning of; this is the one place the meaning is applied.
void write_key(ChunkView& view, u32 row, Entity entity) noexcept {
    auto* keys = static_cast<Entity*>(view.keys());
    keys[row] = entity;
}

[[nodiscard]] Entity read_key(ChunkView& view, u32 row) noexcept {
    return static_cast<const Entity*>(view.keys())[row];
}

}  // namespace

World::World(Allocator& allocator, const WorldConfig& config) noexcept
    : allocator_(&allocator),
      config_(config),
      components_(allocator),
      resources_(allocator),
      entities_(allocator),
      archetypes_(allocator, config.chunk_bytes),
      shared_tables_(allocator),
      sparse_(allocator),
      command_buffers_(allocator),
      scratch_components_(allocator),
      scratch_shared_(allocator),
      scratch_entities_(allocator),
      scratch_ranges_(allocator),
      transitions_(allocator) {}

World::~World() = default;

Status World::initialize() noexcept {
    if (initialized_) {
        return ok();
    }

    // `Parent` declares the byte offset of its one entity reference. Serialization remaps
    // references by walking declared offsets rather than by asking reflection per row what a field
    // means — see component.h.
    static constexpr u32 kParentEntityOffset[] = {0};
    ComponentOptions parent_options;
    parent_options.entity_offsets = Span<const u32>(kParentEntityOffset, 1);
    Expected<ComponentTypeId, Error> parent =
        components_.register_builtin(kParentComponentName, static_cast<u32>(sizeof(Parent)),
                                     static_cast<u32>(alignof(Parent)), parent_options);
    if (!parent) {
        return make_unexpected(parent.error());
    }
    parent_component_ = *parent;

    ComponentOptions children_options;
    children_options.kind = ComponentKind::Buffer;
    children_options.element_size = static_cast<u32>(sizeof(Entity));
    children_options.element_alignment = static_cast<u32>(alignof(Entity));
    children_options.inline_capacity = kInlineChildren;
    children_options.elements_are_entities = true;
    children_options.release = &release_buffer<Entity>;
    Expected<ComponentTypeId, Error> children = components_.register_builtin(
        kChildrenComponentName, 0, static_cast<u32>(alignof(Entity)), children_options);
    if (!children) {
        return make_unexpected(children.error());
    }
    children_component_ = *children;

    initialized_ = true;
    return ok();
}

Status World::admit_structural_change() noexcept {
    if (iterating()) {
        // Counted in every configuration. An assertion here would be compiled out of Profile and
        // Shipping, which are exactly the builds where a system quietly mutating mid-iteration
        // would go unnoticed. See the header.
        ++refused_;
        return fail(
            ErrorCode::Unavailable,
            "structural changes are deferred during iteration; record into a CommandBuffer");
    }
    ++structural_changes_;
    return ok();
}

// --- Entities -------------------------------------------------------------------------------

Expected<Entity, Error> World::create() noexcept {
    return create(Span<const ComponentTypeId>{});
}

Expected<ComponentMask, Error> World::mask_of(Span<const ComponentTypeId> components,
                                              bool allow_shared) const noexcept {
    ComponentMask mask;
    for (const ComponentTypeId component : components) {
        if (!components_.registered(component)) {
            return fail(ErrorCode::NotFound, "this world has not registered that component");
        }
        const ComponentKind kind = components_.info(component).kind;
        if (!allow_shared && kind == ComponentKind::Shared) {
            return fail(ErrorCode::InvalidArgument,
                        "a shared component needs a value; add it with set_shared()");
        }
        if (kind_changes_archetype(kind)) {
            mask.set(component);
        }
    }
    return mask;
}

Expected<Entity, Error> World::create(Span<const ComponentTypeId> components) noexcept {
    if (Status admitted = admit_structural_change(); !admitted) {
        return make_unexpected(admitted.error());
    }

    Expected<ComponentMask, Error> mask = mask_of(components, false);
    if (!mask) {
        return make_unexpected(mask.error());
    }

    Expected<Archetype*, Error> archetype =
        archetypes_.find_or_create(*mask, components, Span<const SharedValue>{}, components_);
    if (!archetype) {
        return make_unexpected(archetype.error());
    }

    Expected<Entity, Error> entity = entities_.create();
    if (!entity) {
        return entity;
    }
    if (Status placed = place(*entity, **archetype); !placed) {
        (void)entities_.destroy(*entity);
        return make_unexpected(placed.error());
    }
    return *entity;
}

Status World::place(Entity entity, Archetype& archetype) noexcept {
    Expected<ChunkStore::RowLocation, Error> row = archetype.add_row(version_);
    if (!row) {
        return make_unexpected(row.error());
    }
    ChunkView view = archetype.chunk(row->chunk);
    write_key(view, row->row, entity);
    entities_.set_location(entity, EntityLocation{archetype.id(), row->chunk, row->row});
    return ok();
}

Status World::create_many(u32 count, Span<const ComponentTypeId> components,
                          Array<Entity>& out) noexcept {
    if (Status admitted = admit_structural_change(); !admitted) {
        return admitted;
    }
    if (count == 0) {
        return ok();
    }

    // The archetype is resolved once for the whole batch. `ecs-core`'s "bulk creation is cheap"
    // scenario is exactly this: no per-entity archetype lookup.
    Expected<ComponentMask, Error> mask = mask_of(components, true);
    if (!mask) {
        return make_unexpected(mask.error());
    }
    Expected<Archetype*, Error> archetype =
        archetypes_.find_or_create(*mask, components, Span<const SharedValue>{}, components_);
    if (!archetype) {
        return make_unexpected(archetype.error());
    }

    if (Status reserved = entities_.reserve(entities_.capacity() + count); !reserved) {
        return reserved;
    }
    if (Status reserved = out.reserve(out.size() + count); !reserved) {
        return reserved;
    }

    scratch_ranges_.clear();
    if (Status added = (*archetype)->add_rows(count, version_, scratch_ranges_); !added) {
        return added;
    }

    for (const Archetype::RowRange& range : scratch_ranges_) {
        if (Status populated = populate_rows(**archetype, range, out); !populated) {
            return populated;
        }
    }
    return ok();
}

Status World::populate_rows(Archetype& archetype, const Archetype::RowRange& range,
                            Array<Entity>& out) noexcept {
    ChunkView view = archetype.chunk(range.chunk);
    for (u32 offset = 0; offset < range.count; ++offset) {
        Expected<Entity, Error> entity = entities_.create();
        if (!entity) {
            return make_unexpected(entity.error());
        }
        const u32 row = range.first_row + offset;
        write_key(view, row, *entity);
        entities_.set_location(*entity, EntityLocation{archetype.id(), range.chunk, row});
        if (Status pushed = out.push_back(*entity); !pushed) {
            return pushed;
        }
    }
    return ok();
}

void World::vacate(Archetype& archetype, u32 chunk, u32 row, const ComponentMask& keep) noexcept {
    archetype.release_buffers_except(chunk, row, keep);
    const ChunkStore::RowMove move = archetype.remove_moved_row(chunk, row);
    if (move.moved) {
        // The last row took the vacated one's place, so the entity that was in it has a new row and
        // its location record is now wrong. Repairing it here is what `ChunkStore::remove_row`
        // reports the move for: the store knows the rows moved and deliberately does not know who
        // was in them.
        ChunkView view = archetype.chunk(move.chunk);
        entities_.set_location(read_key(view, move.to_row),
                               EntityLocation{archetype.id(), move.chunk, move.to_row});
    }
    if (chunk < archetype.chunk_count()) {
        archetype.stamp(chunk, version_);
    }
}

Status World::destroy_one(Entity entity) noexcept {
    const EntityLocation* location = entities_.location(entity);
    if (location == nullptr) {
        return fail(ErrorCode::NotFound, "destroy() on an entity that is not alive");
    }
    if (location->archetype < archetypes_.size()) {
        // An entity exists between `EntityTable::create` and being placed in an archetype. Nothing
        // outside this file can observe that window, but the destroy path is reachable from it: a
        // failed placement destroys the entity it had just created.
        Archetype& archetype = archetypes_.at(location->archetype);
        vacate(archetype, location->chunk, location->row, ComponentMask{});
    }

    // Sparse components are not part of the archetype, so nothing above removed them. A stale
    // entry would be found by a recycled index's generation check rather than returned, but it
    // would also never be reclaimed, so it is erased here.
    for (SparseSlot& slot : sparse_) {
        (void)slot.store.erase(entity);
    }
    return entities_.destroy(entity);
}

Status World::destroy(Entity entity, DestroyPolicy policy) noexcept {
    if (Status admitted = admit_structural_change(); !admitted) {
        return admitted;
    }
    if (!is_alive(entity)) {
        return fail(ErrorCode::NotFound, "destroy() on an entity that is not alive");
    }

    if (policy == DestroyPolicy::ReparentChildren) {
        // The opt-out: the subtree survives, reattached to the destroyed entity's own parent.
        const Entity grandparent = parent_of(entity);
        scratch_entities_.clear();
        if (Status appended = scratch_entities_.append(children_of(entity)); !appended) {
            return appended;
        }
        for (const Entity child : scratch_entities_) {
            if (Status reparented = set_parent(child, grandparent); !reparented) {
                return reparented;
            }
        }
        if (Status detached = detach_from_parent(entity); !detached) {
            return detached;
        }
        return destroy_one(entity);
    }

    // Cascade: the whole subtree in one deferred operation, deepest first so a parent's `Children`
    // is never read after its buffer has been released.
    Array<Entity> subtree(*allocator_);
    if (Status collected = collect_subtree(entity, subtree); !collected) {
        return collected;
    }
    if (Status detached = detach_from_parent(entity); !detached) {
        return detached;
    }
    for (usize index = subtree.size(); index > 0; --index) {
        if (Status destroyed = destroy_one(subtree[index - 1]); !destroyed) {
            return destroyed;
        }
    }
    return ok();
}

Status World::destroy_many(Span<const Entity> entities, DestroyPolicy policy) noexcept {
    for (const Entity entity : entities) {
        if (!is_alive(entity)) {
            // Already gone, whether by an earlier cascade in this same call or before it. Not an
            // error: a bulk destroy of a subtree names entities the cascade has already taken.
            continue;
        }
        if (Status destroyed = destroy(entity, policy); !destroyed) {
            return destroyed;
        }
    }
    return ok();
}

// --- Components on entities -----------------------------------------------------------------

bool World::has(Entity entity, ComponentTypeId component) const noexcept {
    if (!components_.registered(component)) {
        return false;
    }
    if (components_.info(component).kind == ComponentKind::Sparse) {
        const SparseStore* store = sparse_store(component);
        return store != nullptr && store->find(entity) != nullptr;
    }
    const EntityLocation* location = entities_.location(entity);
    return location != nullptr && archetypes_.at(location->archetype).mask().test(component);
}

const void* World::get(Entity entity, ComponentTypeId component) const noexcept {
    const EntityLocation* location = entities_.location(entity);
    if (location == nullptr) {
        return nullptr;
    }
    // const_cast: the archetype table's accessors are non-const because a `ChunkView` is a mutable
    // view by construction — `chunk_storage.h` has one view type, not two. Nothing below writes.
    auto& archetype = const_cast<ArchetypeTable&>(archetypes_).at(location->archetype);
    return archetype.value_at(location->chunk, location->row, component);
}

void* World::get_mut(Entity entity, ComponentTypeId component) noexcept {
    const EntityLocation* location = entities_.location(entity);
    if (location == nullptr) {
        return nullptr;
    }
    Archetype& archetype = archetypes_.at(location->archetype);
    void* value = archetype.value_at(location->chunk, location->row, component);
    if (value != nullptr) {
        // A write through this pointer dirties the chunk for this component, at the world's current
        // version. Read-only access goes through get(), which does not — that is the whole of
        // `ecs-core`'s "read does not dirty".
        const i32 column = archetype.column_of(component);
        ChunkView view = archetype.chunk(location->chunk);
        view.set_version(static_cast<u32>(column), version_);
    }
    return value;
}

Status World::current_set(Entity entity, ComponentMask& mask, Array<ComponentTypeId>& components,
                          Array<SharedValue>& shared) const noexcept {
    const EntityLocation* location = entities_.location(entity);
    if (location == nullptr) {
        return fail(ErrorCode::NotFound, "this entity is not alive");
    }
    const Archetype& archetype = archetypes_.at(location->archetype);
    mask = archetype.mask();
    components.clear();
    shared.clear();
    if (Status appended = components.append(archetype.components()); !appended) {
        return appended;
    }
    return shared.append(archetype.shared());
}

Status World::move_to(Entity entity, const ComponentMask& mask,
                      Span<const ComponentTypeId> components,
                      Span<const SharedValue> shared) noexcept {
    const EntityLocation* location = entities_.location(entity);
    if (location == nullptr) {
        return fail(ErrorCode::NotFound, "this entity is not alive");
    }
    const EntityLocation from = *location;

    Expected<Archetype*, Error> target =
        archetypes_.find_or_create(mask, components, shared, components_);
    if (!target) {
        return make_unexpected(target.error());
    }
    Archetype& source = archetypes_.at(from.archetype);
    if ((*target)->id() == source.id()) {
        return ok();
    }

    Expected<ChunkStore::RowLocation, Error> row = (*target)->add_row(version_);
    if (!row) {
        return make_unexpected(row.error());
    }
    // The transition is a memcpy per shared column and nothing per field. A buffer component's
    // header moves with it, so the heap block it points at changes owner rather than being copied.
    (*target)->copy_shared_columns(source, from.chunk, from.row, row->chunk, row->row);
    ChunkView target_view = (*target)->chunk(row->chunk);
    write_key(target_view, row->row, entity);

    vacate(source, from.chunk, from.row, (*target)->mask());
    entities_.set_location(entity, EntityLocation{(*target)->id(), row->chunk, row->row});
    ++archetype_transitions_;
    last_transition_entity_ = entity;
    count_transition(entity);
    return ok();
}

Status World::add(Entity entity, ComponentTypeId component, const void* value) noexcept {
    if (!components_.registered(component)) {
        return fail(ErrorCode::NotFound, "add() names a component this world has not registered");
    }
    const ComponentInfo& info = components_.info(component);
    if (info.kind == ComponentKind::Sparse) {
        return set_sparse(entity, component, value);
    }
    if (info.kind == ComponentKind::Shared) {
        return fail(ErrorCode::InvalidArgument,
                    "a shared component carries a value; add it with set_shared()");
    }
    if (Status admitted = admit_structural_change(); !admitted) {
        return admitted;
    }

    ComponentMask mask;
    if (Status current = current_set(entity, mask, scratch_components_, scratch_shared_);
        !current) {
        return current;
    }
    if (!mask.test(component)) {
        mask.set(component);
        if (Status pushed = scratch_components_.push_back(component); !pushed) {
            return pushed;
        }
        last_transition_component_ = component;
        if (Status moved =
                move_to(entity, mask, scratch_components_.span(), scratch_shared_.span());
            !moved) {
            return moved;
        }
    }

    if (value != nullptr && info.size != 0) {
        void* slot = get_mut(entity, component);
        if (slot == nullptr) {
            return fail(ErrorCode::Internal, "the component was added but has no storage");
        }
        std::memcpy(slot, value, info.size);
    }
    return ok();
}

Status World::remove(Entity entity, ComponentTypeId component) noexcept {
    if (!components_.registered(component)) {
        return fail(ErrorCode::NotFound, "remove() names an unregistered component");
    }
    if (components_.info(component).kind == ComponentKind::Sparse) {
        return remove_sparse(entity, component);
    }
    if (Status admitted = admit_structural_change(); !admitted) {
        return admitted;
    }

    ComponentMask mask;
    if (Status current = current_set(entity, mask, scratch_components_, scratch_shared_);
        !current) {
        return current;
    }
    if (!mask.test(component)) {
        return fail(ErrorCode::NotFound, "this entity does not have that component");
    }
    mask.clear(component);
    for (usize index = 0; index < scratch_components_.size(); ++index) {
        if (scratch_components_[index] == component) {
            scratch_components_.erase(index);
            break;
        }
    }
    for (usize index = 0; index < scratch_shared_.size(); ++index) {
        if (scratch_shared_[index].component == component) {
            scratch_shared_.erase(index);
            break;
        }
    }
    last_transition_component_ = component;
    return move_to(entity, mask, scratch_components_.span(), scratch_shared_.span());
}

// --- Shared components ------------------------------------------------------------------------

Expected<u32, Error> World::intern_shared(ComponentTypeId component, const void* value) noexcept {
    if (!components_.registered(component) ||
        components_.info(component).kind != ComponentKind::Shared) {
        return fail(ErrorCode::InvalidArgument, "that component is not a shared component");
    }
    const u32 size = components_.info(component).value_size;
    while (shared_tables_.size() <= component) {
        SharedTable table(*allocator_);
        if (Status pushed = shared_tables_.push_back(std::move(table)); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    SharedTable& table = shared_tables_[component];
    table.value_size = size;

    for (u32 index = 0; index < table.count; ++index) {
        if (std::memcmp(table.bytes.data() + (usize{index} * size), value, size) == 0) {
            return index;
        }
    }
    const usize offset = table.bytes.size();
    if (Status resized = table.bytes.resize(offset + size); !resized) {
        return make_unexpected(resized.error());
    }
    std::memcpy(static_cast<void*>(table.bytes.data() + offset), value, size);
    return table.count++;
}

const void* World::shared_value(ComponentTypeId component, u32 index) const noexcept {
    if (component >= shared_tables_.size()) {
        return nullptr;
    }
    const SharedTable& table = shared_tables_[component];
    if (index >= table.count) {
        return nullptr;
    }
    return static_cast<const void*>(table.bytes.data() + (usize{index} * table.value_size));
}

Expected<u32, Error> World::shared_of(Entity entity, ComponentTypeId component) const noexcept {
    const EntityLocation* location = entities_.location(entity);
    if (location == nullptr) {
        return fail(ErrorCode::NotFound, "this entity is not alive");
    }
    for (const SharedValue& shared : archetypes_.at(location->archetype).shared()) {
        if (shared.component == component) {
            return shared.value;
        }
    }
    return fail(ErrorCode::NotFound, "this entity does not have that shared component");
}

Status World::set_shared(Entity entity, ComponentTypeId component, u32 value) noexcept {
    if (!components_.registered(component) ||
        components_.info(component).kind != ComponentKind::Shared) {
        return fail(ErrorCode::InvalidArgument, "that component is not a shared component");
    }
    if (shared_value(component, value) == nullptr) {
        return fail(ErrorCode::NotFound, "that shared value has not been interned");
    }
    if (Status admitted = admit_structural_change(); !admitted) {
        return admitted;
    }

    ComponentMask mask;
    if (Status current = current_set(entity, mask, scratch_components_, scratch_shared_);
        !current) {
        return current;
    }
    bool replaced = false;
    for (SharedValue& shared : scratch_shared_) {
        if (shared.component == component) {
            shared.value = value;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        mask.set(component);
        if (Status pushed = scratch_components_.push_back(component); !pushed) {
            return pushed;
        }
        if (Status pushed = scratch_shared_.push_back(SharedValue{component, value}); !pushed) {
            return pushed;
        }
        // The shared list is kept in component order so that two archetypes with the same values
        // compare equal whatever order they were built in.
        for (usize index = scratch_shared_.size() - 1; index > 0; --index) {
            if (scratch_shared_[index - 1].component <= scratch_shared_[index].component) {
                break;
            }
            const SharedValue moved = scratch_shared_[index - 1];
            scratch_shared_[index - 1] = scratch_shared_[index];
            scratch_shared_[index] = moved;
        }
    }
    last_transition_component_ = component;
    return move_to(entity, mask, scratch_components_.span(), scratch_shared_.span());
}

// --- Sparse components -------------------------------------------------------------------------

Status World::ensure_sparse_store(ComponentTypeId component) noexcept {
    if (sparse_store(component) != nullptr) {
        return ok();
    }
    const ComponentInfo& info = components_.info(component);
    SparseSlot slot(*allocator_, component, info.value_size);
    return sparse_.push_back(std::move(slot));
}

SparseStore* World::sparse_store(ComponentTypeId component) noexcept {
    for (SparseSlot& slot : sparse_) {
        if (slot.component == component) {
            return &slot.store;
        }
    }
    return nullptr;
}

const SparseStore* World::sparse_store(ComponentTypeId component) const noexcept {
    for (const SparseSlot& slot : sparse_) {
        if (slot.component == component) {
            return &slot.store;
        }
    }
    return nullptr;
}

Status World::set_sparse(Entity entity, ComponentTypeId component, const void* value) noexcept {
    if (!components_.registered(component) ||
        components_.info(component).kind != ComponentKind::Sparse) {
        return fail(ErrorCode::InvalidArgument, "that component is not a sparse component");
    }
    if (!is_alive(entity)) {
        return fail(ErrorCode::NotFound, "this entity is not alive");
    }
    // Deliberately NOT a structural change: the archetype does not move, no row is copied, and no
    // chunk a query is walking is touched. That is the entire reason `ecs-core` offers the kind,
    // and it is why this call is legal during iteration.
    SparseStore* store = sparse_store(component);
    if (store == nullptr) {
        // Creating the side table is not: it grows `sparse_`, which would move every other store
        // under whichever parallel system is writing into one. Refused rather than raced — the
        // table is created by the first write outside a stage, which is where a component's storage
        // should be brought into existence anyway.
        if (iterating()) {
            return fail(ErrorCode::Unavailable,
                        "a sparse component's side table must exist before the stage runs; write "
                        "it once outside iteration first");
        }
        if (Status ensured = ensure_sparse_store(component); !ensured) {
            return ensured;
        }
        store = sparse_store(component);
    }
    return store->set(entity, value);
}

void* World::get_sparse(Entity entity, ComponentTypeId component) noexcept {
    SparseStore* store = sparse_store(component);
    return (store == nullptr) ? nullptr : store->find(entity);
}

const void* World::get_sparse(Entity entity, ComponentTypeId component) const noexcept {
    const SparseStore* store = sparse_store(component);
    return (store == nullptr) ? nullptr : store->find(entity);
}

Status World::remove_sparse(Entity entity, ComponentTypeId component) noexcept {
    SparseStore* store = sparse_store(component);
    if (store == nullptr) {
        return fail(ErrorCode::NotFound, "that sparse component has no entries");
    }
    return store->erase(entity);
}

// --- Bulk instantiation --------------------------------------------------------------------------

void World::copy_block_columns(Archetype& archetype, const ArchetypeBlock& block,
                               const Archetype::RowRange& range, u32 consumed) noexcept {
    ChunkView view = archetype.chunk(range.chunk);
    // One memcpy per column per run: this is the bulk copy `ecs-core`'s "bulk instantiation"
    // scenario names, with no per-entity construction and no reflection anywhere on the path.
    for (usize index = 0; index < block.components.size(); ++index) {
        const i32 column = archetype.column_of(block.components[index]);
        if (column < 0 || block.columns[index] == nullptr) {
            continue;
        }
        const auto size = static_cast<usize>(view.layout().column_size(static_cast<u32>(column)));
        auto* target = static_cast<u8*>(view.column(static_cast<u32>(column)));
        const auto* source = static_cast<const u8*>(block.columns[index]);
        std::memcpy(static_cast<void*>(target + (size * range.first_row)),
                    static_cast<const void*>(source + (size * consumed)), size * range.count);
    }
}

Status World::instantiate(const ArchetypeBlock& block, Array<Entity>& out) noexcept {
    if (Status admitted = admit_structural_change(); !admitted) {
        return admitted;
    }
    if (block.components.size() != block.columns.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "an archetype block needs one column per named component");
    }
    if (block.count == 0) {
        return ok();
    }

    Expected<ComponentMask, Error> mask = mask_of(block.components, true);
    if (!mask) {
        return make_unexpected(mask.error());
    }
    Expected<Archetype*, Error> archetype =
        archetypes_.find_or_create(*mask, block.components, block.shared, components_);
    if (!archetype) {
        return make_unexpected(archetype.error());
    }

    if (Status reserved = out.reserve(out.size() + block.count); !reserved) {
        return reserved;
    }
    scratch_ranges_.clear();
    if (Status added = (*archetype)->add_rows(block.count, version_, scratch_ranges_); !added) {
        return added;
    }

    u32 consumed = 0;
    for (const Archetype::RowRange& range : scratch_ranges_) {
        copy_block_columns(**archetype, block, range, consumed);
        if (Status populated = populate_rows(**archetype, range, out); !populated) {
            return populated;
        }
        consumed += range.count;
    }
    return ok();
}

// --- Statistics --------------------------------------------------------------------------------

WorldStats World::stats() const noexcept {
    WorldStats stats;
    stats.archetypes = archetypes_.size();
    stats.entities = entities_.live_count();
    stats.structural_changes = structural_changes();
    stats.archetype_transitions = archetype_transitions_;
    stats.refused_during_iteration = refused_during_iteration();
    auto& table = const_cast<ArchetypeTable&>(archetypes_);
    for (u32 index = 0; index < table.size(); ++index) {
        Archetype& archetype = table.at(index);
        stats.chunks += archetype.chunk_count();
        stats.chunk_capacity += u64{archetype.chunk_count()} * archetype.capacity();
        stats.committed_bytes += archetype.store().committed_bytes();
    }
    if (stats.chunk_capacity != 0) {
        stats.fill_ratio =
            static_cast<f64>(stats.entities) / static_cast<f64>(stats.chunk_capacity);
    }
    return stats;
}

usize World::trim() noexcept {
    return archetypes_.trim();
}

void World::count_transition(Entity entity) noexcept {
    const usize wanted = usize{entity.index()} + 1;
    while (transitions_.size() < wanted) {
        if (Status pushed = transitions_.push_back(TransitionCounter{}); !pushed) {
            // The counter is a diagnostic. Failing to grow it must not fail the transition that
            // was asked for, so the count is simply not recorded.
            return;
        }
    }
    TransitionCounter& counter = transitions_[entity.index()];
    ++counter.count;
    counter.component = last_transition_component_;
}

World::TransitionSample World::busiest_entity() const noexcept {
    TransitionSample sample;
    for (usize index = 0; index < transitions_.size(); ++index) {
        if (transitions_[index].count <= sample.transitions) {
            continue;
        }
        sample.transitions = transitions_[index].count;
        sample.component = transitions_[index].component;
        sample.entity = entities_.at(static_cast<u32>(index));
    }
    return sample;
}

void World::reset_transition_counters() noexcept {
    for (TransitionCounter& counter : transitions_) {
        counter = TransitionCounter{};
    }
}

}  // namespace cy::ecs
