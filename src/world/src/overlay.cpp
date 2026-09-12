#include <cy/world/overlay.h>

#include <algorithm>
#include <ranges>

namespace cy::world {

const char* migration_policy_name(MigrationPolicy policy) noexcept {
    switch (policy) {
        case MigrationPolicy::Migrate:
            return "Migrate";
        case MigrationPolicy::BecomeRuntimeManaged:
            return "BecomeRuntimeManaged";
        case MigrationPolicy::PersistAndRemove:
            return "PersistAndRemove";
    }
    return "unknown";
}

bool CellOverlay::empty() const noexcept {
    return removed.empty() && overrides.empty() && positions.empty() && blobs.empty() &&
           created.blocks.empty();
}

PersistenceOverlay::PersistenceOverlay(Allocator& allocator) noexcept
    : allocator_(&allocator),
      cells_(allocator),
      values_(allocator),
      layer_states_(allocator),
      variables_(allocator) {}

Status PersistenceOverlay::check_content_version(u64 installed) const noexcept {
    if (content_version_ == installed) {
        return ok();
    }
    // Detected and reported, not silently applied. `save-and-persistence` owns the migration that
    // follows; the world's job is to refuse to apply an overlay to content it was not written
    // against, because doing so corrupts a save rather than failing one.
    return fail(ErrorCode::Unsupported,
                "this overlay was produced against a different content version; migrate it rather "
                "than applying it");
}

Expected<CellOverlay*, Error> PersistenceOverlay::entry_for(CellId cell) noexcept {
    for (CellOverlay& entry : cells_.span()) {
        if (entry.cell == cell) {
            return &entry;
        }
    }
    CellOverlay entry(*allocator_);
    entry.cell = cell;
    if (Status pushed = cells_.push_back(std::move(entry)); !pushed) {
        return make_unexpected(pushed.error());
    }
    return &cells_[cells_.size() - 1];
}

Status PersistenceOverlay::record_removed(CellId cell, PersistentId entity) noexcept {
    Expected<CellOverlay*, Error> overlay = entry_for(cell);
    if (!overlay) {
        return Status{make_unexpected(overlay.error())};
    }
    for (const PersistentId existing : (*overlay)->removed.span()) {
        if (existing == entity) {
            return ok();
        }
    }
    return (*overlay)->removed.push_back(entity);
}

Status PersistenceOverlay::record_component(CellId cell, PersistentId entity,
                                            ecs::ComponentTypeId component,
                                            Span<const u8> value) noexcept {
    Expected<CellOverlay*, Error> overlay = entry_for(cell);
    if (!overlay) {
        return Status{make_unexpected(overlay.error())};
    }

    const auto first = static_cast<u32>(values_.size());
    if (Status appended = values_.append(value); !appended) {
        return appended;
    }

    // An override REPLACES the previous one for the same (entity, component). The old bytes stay in
    // the pool: compacting them would move every other record's offset, and the pool is bounded by
    // how much of the world has been changed, not by how long the session has run.
    for (ComponentOverride& existing : (*overlay)->overrides.span()) {
        if (existing.entity == entity && existing.component == component) {
            existing.first = first;
            existing.size = static_cast<u32>(value.size());
            return ok();
        }
    }
    return (*overlay)->overrides.push_back(
        ComponentOverride{entity, component, first, static_cast<u32>(value.size())});
}

Status PersistenceOverlay::record_blob(CellId cell, u32 channel, u64 key,
                                       Span<const u8> value) noexcept {
    Expected<CellOverlay*, Error> overlay = entry_for(cell);
    if (!overlay) {
        return Status{make_unexpected(overlay.error())};
    }

    const auto first = static_cast<u32>(values_.size());
    if (Status appended = values_.append(value); !appended) {
        return appended;
    }

    // Replaces the previous blob for the same (channel, key), for the reason a component override
    // does: the old bytes stay in the pool because compacting them would move every other record's
    // offset, and the pool is bounded by how much of the world has changed.
    for (OverlayBlob& existing : (*overlay)->blobs.span()) {
        if (existing.channel == channel && existing.key == key) {
            existing.first = first;
            existing.size = static_cast<u32>(value.size());
            return ok();
        }
    }
    return (*overlay)->blobs.push_back(
        OverlayBlob{channel, key, first, static_cast<u32>(value.size())});
}

Status PersistenceOverlay::record_created(CellId cell, PersistentId entity, LayerId layer,
                                          Span<const ecs::ComponentTypeId> components,
                                          Span<const void* const> values,
                                          Span<const u32> sizes) noexcept {
    Expected<CellOverlay*, Error> overlay = entry_for(cell);
    if (!overlay) {
        return Status{make_unexpected(overlay.error())};
    }
    (*overlay)->created.id = cell;
    return append_cooked_row(*allocator_, (*overlay)->created, entity, layer, components, values,
                             sizes);
}

Status PersistenceOverlay::record_position(CellId cell, PersistentId entity,
                                           const WorldPosition& position) noexcept {
    Expected<CellOverlay*, Error> overlay = entry_for(cell);
    if (!overlay) {
        return Status{make_unexpected(overlay.error())};
    }
    for (PositionRecord& record : (*overlay)->positions.span()) {
        if (record.entity == entity) {
            record.position = position;
            return ok();
        }
    }
    return (*overlay)->positions.push_back(PositionRecord{entity, position});
}

Status PersistenceOverlay::record_layer_state(LayerId layer, LayerState state) noexcept {
    for (LayerTable::StateChange& change : layer_states_.span()) {
        if (change.layer == layer) {
            change.state = state;
            return ok();
        }
    }
    return layer_states_.push_back(LayerTable::StateChange{layer, state});
}

Status PersistenceOverlay::set_variable(u64 key, i64 value) noexcept {
    for (WorldVariable& variable : variables_.span()) {
        if (variable.key == key) {
            variable.integer = value;
            return ok();
        }
    }
    return variables_.push_back(WorldVariable{key, value, 0.0});
}

Status PersistenceOverlay::set_real_variable(u64 key, f64 value) noexcept {
    for (WorldVariable& variable : variables_.span()) {
        if (variable.key == key) {
            variable.real = value;
            return ok();
        }
    }
    return variables_.push_back(WorldVariable{key, 0, value});
}

const CellOverlay* PersistenceOverlay::find(CellId cell) const noexcept {
    for (const CellOverlay& entry : cells_.span()) {
        if (entry.cell == cell) {
            return &entry;
        }
    }
    return nullptr;
}

bool PersistenceOverlay::is_removed(CellId cell, PersistentId entity) const noexcept {
    const CellOverlay* overlay = find(cell);
    if (overlay == nullptr) {
        return false;
    }
    const Span<const PersistentId> removed = overlay->removed.span();
    return std::ranges::any_of(removed,
                               [entity](PersistentId candidate) { return candidate == entity; });
}

Span<const u8> PersistenceOverlay::component_override(
    CellId cell, PersistentId entity, ecs::ComponentTypeId component) const noexcept {
    const CellOverlay* overlay = find(cell);
    if (overlay == nullptr) {
        return {};
    }
    for (const ComponentOverride& record : overlay->overrides.span()) {
        if (record.entity == entity && record.component == component) {
            return values_.span().subspan(record.first, record.size);
        }
    }
    return {};
}

Span<const u8> PersistenceOverlay::blob(CellId cell, u32 channel, u64 key) const noexcept {
    const CellOverlay* overlay = find(cell);
    if (overlay == nullptr) {
        return {};
    }
    for (const OverlayBlob& record : overlay->blobs.span()) {
        if (record.channel == channel && record.key == key) {
            return values_.span().subspan(record.first, record.size);
        }
    }
    return {};
}

const WorldVariable* PersistenceOverlay::variable(u64 key) const noexcept {
    for (const WorldVariable& variable : variables_.span()) {
        if (variable.key == key) {
            return &variable;
        }
    }
    return nullptr;
}

Status PersistenceOverlay::cells(Array<CellId>& out) const noexcept {
    const usize first = out.size();
    for (const CellOverlay& entry : cells_.span()) {
        if (Status pushed = out.push_back(entry.cell); !pushed) {
            return pushed;
        }
    }
    // Sorted, because a save of one world state must be the same sequence however that state was
    // reached, and the insertion order of the overlay is the order the player happened to play in.
    std::sort(out.data() + first, out.data() + out.size());
    return ok();
}

// --- The dynamic index -------------------------------------------------------------------------

DynamicIndex::DynamicIndex(Allocator& allocator) noexcept : entities_(allocator) {}

Status DynamicIndex::track(PersistentId entity, CellId home, const WorldPosition& position,
                           MigrationPolicy policy) noexcept {
    if (Tracked* existing = mutable_find(entity); existing != nullptr) {
        existing->home = home;
        existing->position = position;
        existing->runtime_cell = position.cell;
        existing->policy = policy;
        return ok();
    }
    Tracked tracked;
    tracked.entity = entity;
    tracked.home = home;
    tracked.position = position;
    tracked.runtime_cell = position.cell;
    tracked.policy = policy;
    return entities_.push_back(tracked);
}

Status DynamicIndex::forget(PersistentId entity) noexcept {
    for (usize index = 0; index < entities_.size(); ++index) {
        if (entities_[index].entity != entity) {
            continue;
        }
        for (usize shift = index + 1; shift < entities_.size(); ++shift) {
            entities_[shift - 1] = entities_[shift];
        }
        entities_.pop_back();
        return ok();
    }
    return fail(ErrorCode::NotFound, "that entity is not in the dynamic index");
}

Status DynamicIndex::moved(PersistentId entity, const WorldPosition& position) noexcept {
    Tracked* tracked = mutable_find(entity);
    if (tracked == nullptr) {
        return fail(ErrorCode::NotFound, "that entity is not in the dynamic index");
    }
    if (!(tracked->runtime_cell == position.cell)) {
        ++tracked->crossings;
        tracked->runtime_cell = position.cell;
    }
    tracked->position = position;
    // `home` is NOT touched. A vehicle driving across a hundred cells rewrites no persistent
    // ownership, and `crossings` is what a test reads to show that it crossed them.
    return ok();
}

const DynamicIndex::Tracked* DynamicIndex::find(PersistentId entity) const noexcept {
    for (const Tracked& tracked : entities_.span()) {
        if (tracked.entity == entity) {
            return &tracked;
        }
    }
    return nullptr;
}

DynamicIndex::Tracked* DynamicIndex::mutable_find(PersistentId entity) noexcept {
    for (Tracked& tracked : entities_.span()) {
        if (tracked.entity == entity) {
            return &tracked;
        }
    }
    return nullptr;
}

Status DynamicIndex::checkpoint(const PartitionConfig& config,
                                PersistenceOverlay& overlay) noexcept {
    for (const Tracked& tracked : entities_.span()) {
        // Recorded against the entity's HOME cell, not the cell it is standing in: the overlay is
        // organised so that an unloaded region's state is available without loading it, and a
        // record filed under wherever the entity wandered would defeat that.
        const WorldPosition normal = normalized(config, tracked.position);
        if (Status recorded = overlay.record_position(tracked.home, tracked.entity, normal);
            !recorded) {
            return recorded;
        }
    }
    return ok();
}

Status DynamicIndex::occupants_of(const PartitionConfig& config, CellId cell,
                                  Array<PersistentId>& out) const noexcept {
    for (const Tracked& tracked : entities_.span()) {
        if (cell_id_of(config, tracked.runtime_cell) == cell) {
            if (Status pushed = out.push_back(tracked.entity); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

}  // namespace cy::world
