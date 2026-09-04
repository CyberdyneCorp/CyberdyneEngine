#include <cy/rendering/culling/spatial.h>

#include <cy/core/base/assert.h>
#include <cy/core/math/vec.h>

namespace cy::rendering {
namespace {

/// The world bounding sphere's radius for a box. Derived once at insert because screen coverage
/// needs it per view per frame, and a square root per instance per view is a measurable cost in a
/// scene whose whole point is that the broad phase touches minimal memory.
[[nodiscard]] f32 radius_of(const Aabb& bounds) noexcept {
    if (bounds.is_empty()) {
        return 0.0F;
    }
    return length(bounds.half_extents());
}

}  // namespace

SpatialIndex::SpatialIndex(Allocator& allocator) noexcept
    : bounds_(allocator),
      flags_(allocator),
      masks_(allocator),
      entries_(allocator),
      proxies_(allocator),
      free_slots_(allocator),
      always_visible_(allocator) {}

bool SpatialIndex::valid(u32 slot) const noexcept {
    return slot < bounds_.size() && (flags_[slot] & kSpatialActive) != 0U;
}

DynamicBvh& SpatialIndex::tree_for(SpatialDomain domain) noexcept {
    return domain == SpatialDomain::Volume ? volumes_ : renderables_;
}

const DynamicBvh& SpatialIndex::tree(SpatialDomain domain) const noexcept {
    return domain == SpatialDomain::Volume ? volumes_ : renderables_;
}

const SpatialEntry& SpatialIndex::entry(u32 slot) const noexcept {
    CY_ASSERT_MSG(slot < entries_.size(), "spatial slot out of range");
    return entries_[slot];
}

Expected<u32, Error> SpatialIndex::insert(const SpatialEntry& entry) noexcept {
    SpatialEntry stored = entry;
    stored.flags |= kSpatialActive;
    stored.radius = radius_of(stored.bounds);

    u32 slot = 0;
    if (!free_slots_.empty()) {
        slot = free_slots_[free_slots_.size() - 1];
        free_slots_.pop_back();
        bounds_[slot] = stored.bounds;
        flags_[slot] = stored.flags;
        masks_[slot] = stored.layer_mask;
        entries_[slot] = stored;
        proxies_[slot] = kNullBvhNode;
    } else {
        slot = static_cast<u32>(bounds_.size());
        if (Status pushed = bounds_.push_back(stored.bounds); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = flags_.push_back(stored.flags); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = masks_.push_back(stored.layer_mask); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = entries_.push_back(stored); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = proxies_.push_back(kNullBvhNode); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    // An always-visible instance bypasses spatial culling, so it is in the flat array and in no
    // tree. That is the requirement's own division and it is why there are three containers rather
    // than one with a flag tested during traversal.
    if ((stored.flags & kSpatialAlwaysVisible) != 0U) {
        if (Status pushed = always_visible_.push_back(slot); !pushed) {
            return make_unexpected(pushed.error());
        }
        return slot;
    }

    Expected<u32, Error> proxy = tree_for(stored.domain).insert(stored.bounds, slot);
    if (!proxy.has_value()) {
        return make_unexpected(proxy.error());
    }
    proxies_[slot] = *proxy;
    return slot;
}

Status SpatialIndex::update(u32 slot, const Aabb& bounds) noexcept {
    if (!valid(slot)) {
        return fail(ErrorCode::NotFound, "spatial slot is not live");
    }
    ++stats_.updates;
    bounds_[slot] = bounds;
    entries_[slot].bounds = bounds;
    entries_[slot].radius = radius_of(bounds);
    if (proxies_[slot] == kNullBvhNode) {
        return ok();
    }
    // The tree's own answer to "did this cost anything": true when the fat bounds no longer
    // contained the new box and the tree had to be restructured.
    Expected<bool, Error> moved = tree_for(entries_[slot].domain).update(proxies_[slot], bounds);
    if (!moved.has_value()) {
        return make_unexpected(moved.error());
    }
    if (*moved) {
        ++stats_.tree_restructures;
    }
    return ok();
}

Status SpatialIndex::set_flags(u32 slot, u32 flags) noexcept {
    if (!valid(slot)) {
        return fail(ErrorCode::NotFound, "spatial slot is not live");
    }
    // `kSpatialAlwaysVisible` decides which container the slot lives in, so it cannot be changed
    // here: doing so would leave the flat array and the tree disagreeing about one instance.
    const u32 container_bits = kSpatialActive | kSpatialAlwaysVisible;
    const u32 merged = (flags & ~container_bits) | (flags_[slot] & container_bits);
    flags_[slot] = merged;
    entries_[slot].flags = merged;
    return ok();
}

Status SpatialIndex::set_layer_mask(u32 slot, render::LayerMask mask) noexcept {
    if (!valid(slot)) {
        return fail(ErrorCode::NotFound, "spatial slot is not live");
    }
    masks_[slot] = mask;
    entries_[slot].layer_mask = mask;
    return ok();
}

Status SpatialIndex::remove(u32 slot) noexcept {
    if (!valid(slot)) {
        return fail(ErrorCode::NotFound, "spatial slot is not live");
    }
    if (proxies_[slot] != kNullBvhNode) {
        if (Status removed = tree_for(entries_[slot].domain).remove(proxies_[slot]); !removed) {
            return removed;
        }
        proxies_[slot] = kNullBvhNode;
    } else {
        for (usize index = 0; index < always_visible_.size(); ++index) {
            if (always_visible_[index] == slot) {
                always_visible_.remove_unordered(index);
                break;
            }
        }
    }
    flags_[slot] = 0;
    masks_[slot] = render::kNoLayers;
    entries_[slot] = SpatialEntry{};
    entries_[slot].flags = 0;
    bounds_[slot] = Aabb::empty();
    return free_slots_.push_back(slot);
}

SpatialStatistics SpatialIndex::statistics() const noexcept {
    SpatialStatistics stats = stats_;
    stats.renderables = static_cast<u32>(renderables_.size());
    stats.volumes = static_cast<u32>(volumes_.size());
    stats.always_visible = static_cast<u32>(always_visible_.size());
    stats.free_slots = static_cast<u32>(free_slots_.size());
    return stats;
}

void SpatialIndex::reset() noexcept {
    bounds_.clear();
    flags_.clear();
    masks_.clear();
    entries_.clear();
    proxies_.clear();
    free_slots_.clear();
    always_visible_.clear();
    renderables_.clear();
    volumes_.clear();
    stats_ = SpatialStatistics{};
}

}  // namespace cy::rendering
