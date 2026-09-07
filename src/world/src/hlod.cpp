#include <cy/world/hlod.h>

namespace cy::world {

HlodRegistry::HlodRegistry(Allocator& allocator) noexcept
    : proxies_(allocator), covered_(allocator) {}

Status HlodRegistry::declare(CellId cell, AssetId asset, u8 level,
                             Span<const CellId> covered) noexcept {
    if (!cell.is_valid()) {
        return fail(ErrorCode::InvalidArgument, "an HLOD proxy stands in for a cell");
    }
    HlodProxy proxy;
    proxy.cell = cell;
    proxy.asset = asset;
    proxy.level = level;
    proxy.first_covered = static_cast<u32>(covered_.size());
    proxy.covered_count = static_cast<u32>(covered.size());
    // Visible from the moment it is declared: a region nobody has activated yet is a region the
    // proxy is standing in for, and starting hidden would make the first frame the one with a hole
    // in it.
    proxy.visible = true;
    if (Status appended = covered_.append(covered); !appended) {
        return appended;
    }
    for (HlodProxy& existing : proxies_.span()) {
        if (existing.cell == cell) {
            const bool was_visible = existing.visible;
            existing = proxy;
            existing.visible = was_visible;
            return ok();
        }
    }
    return proxies_.push_back(proxy);
}

const HlodProxy* HlodRegistry::find(CellId cell) const noexcept {
    for (const HlodProxy& proxy : proxies_.span()) {
        if (proxy.cell == cell) {
            return &proxy;
        }
    }
    return nullptr;
}

bool HlodRegistry::is_visible(CellId cell) const noexcept {
    const HlodProxy* proxy = find(cell);
    return proxy != nullptr && proxy->visible;
}

u32 HlodRegistry::visible_count() const noexcept {
    u32 count = 0;
    for (const HlodProxy& proxy : proxies_.span()) {
        count += proxy.visible ? 1u : 0u;
    }
    return count;
}

const char* representation_tier_name(RepresentationTier tier) noexcept {
    switch (tier) {
        case RepresentationTier::Full:
            return "full";
        case RepresentationTier::Aggregate:
            return "aggregate";
        case RepresentationTier::Statistical:
            return "statistical";
    }
    return "unknown";
}

RepresentationTable::RepresentationTable(Allocator& allocator) noexcept : groups_(allocator) {}

Status RepresentationTable::add(const Representation& representation) noexcept {
    if (!representation.id.is_valid()) {
        return fail(ErrorCode::InvalidArgument, "a representation carries a persistent identity");
    }
    if (Representation* existing = mutable_find(representation.id); existing != nullptr) {
        *existing = representation;
        return ok();
    }
    return groups_.push_back(representation);
}

const Representation* RepresentationTable::find(PersistentId id) const noexcept {
    for (const Representation& group : groups_.span()) {
        if (group.id == id) {
            return &group;
        }
    }
    return nullptr;
}

Representation* RepresentationTable::mutable_find(PersistentId id) noexcept {
    for (Representation& group : groups_.span()) {
        if (group.id == id) {
            return &group;
        }
    }
    return nullptr;
}

Status RepresentationTable::set_tier(PersistentId id, RepresentationTier tier) noexcept {
    Representation* group = mutable_find(id);
    if (group == nullptr) {
        return fail(ErrorCode::NotFound, "no representation with that identity");
    }
    if (group->tier == tier) {
        return ok();
    }
    group->tier = tier;
    // `population` and `gameplay_state` are NOT touched. That is the requirement — "promotion and
    // demotion SHALL preserve identity and gameplay-relevant state, so an army that is demoted and
    // later promoted has not silently changed" — and the assignment that would break it is the one
    // that is deliberately absent here.
    group->materialised = (tier == RepresentationTier::Full) ? group->population : 0;
    return ok();
}

Status RepresentationTable::set_position(PersistentId id, const WorldPosition& position) noexcept {
    Representation* group = mutable_find(id);
    if (group == nullptr) {
        return fail(ErrorCode::NotFound, "no representation with that identity");
    }
    group->position = position;
    return ok();
}

}  // namespace cy::world
