// The snapshot, as the index a cull reads. See cy/rendering/assembly/scene_index.h.

#include <cy/rendering/assembly/scene_index.h>

namespace cy::rendering::assembly {
namespace {

/// The spatial flags one instance's snapshot flags mean.
///
/// A TRANSLATION AND NOT A COPY: the two bit sets are different vocabularies for different
/// questions — `render::InstanceFlagBits` is what a shader reads and `kSpatial*` is what the broad
/// phase tests — and the day one gains a bit the other does not, this function is where that shows.
[[nodiscard]] u32 spatial_flags_of(u32 instance_flags, bool moved) noexcept {
    u32 flags = 0;
    if ((instance_flags & render::kInstanceActive) != 0U) {
        flags |= kSpatialActive;
    }
    if ((instance_flags & render::kInstanceVisible) != 0U) {
        flags |= kSpatialVisible;
    }
    if ((instance_flags & render::kInstanceCastsShadow) != 0U) {
        flags |= kSpatialCastsShadow;
    }
    if (moved) {
        flags |= kSpatialMoved;
    } else {
        flags |= kSpatialStatic;
    }
    return flags;
}

/// The world bounds of an instance: its local box, placed.
[[nodiscard]] Aabb world_bounds_of(const Aabb& local, const Transform& placement) noexcept {
    // Eight corners rather than the transformed min and max: a rotated box's axis-aligned bound is
    // not the bound of its transformed corners taken two at a time, and the cheap version is wrong
    // by up to the box's own diagonal — which is a popping cull at every rotation.
    Aabb bounds = Aabb::empty();
    for (u32 corner = 0; corner < 8; ++corner) {
        bounds.grow(placement.transform_point(local.corner(corner)));
    }
    return bounds;
}

}  // namespace

SceneIndex::SceneIndex(Allocator& allocator) noexcept
    : index_(allocator), entries_(allocator), slot_to_entry_(allocator) {}

SceneIndex::Entry* SceneIndex::find(u64 stable_id) noexcept {
    for (Entry& entry : entries_) {
        if (entry.stable_id == stable_id && entry.slot != kNoSlot) {
            return &entry;
        }
    }
    return nullptr;
}

u32 SceneIndex::slot_of(u64 stable_id) const noexcept {
    for (const Entry& entry : entries_) {
        if (entry.stable_id == stable_id && entry.slot != kNoSlot) {
            return entry.slot;
        }
    }
    return kNoSlot;
}

u64 SceneIndex::id_of(u32 slot) const noexcept {
    if (slot >= slot_to_entry_.size() || slot_to_entry_[slot] == kNoSlot) {
        return 0;
    }
    return entries_[slot_to_entry_[slot]].stable_id;
}

SceneIndex::Surface SceneIndex::surface_of(u32 slot) const noexcept {
    if (slot >= slot_to_entry_.size() || slot_to_entry_[slot] == kNoSlot) {
        return Surface{};
    }
    return entries_[slot_to_entry_[slot]].surface;
}

Status SceneIndex::apply(const render::RenderSnapshot& snapshot, f32 alpha,
                         SceneIndexReport& out) noexcept {
    for (const render::InstanceSnapshot& instance : snapshot.changed) {
        const Transform placement = render::resolve_transform(
            instance.previous_transform, instance.transform, alpha, instance.teleported);
        const bool moved =
            instance.previous_transform.translation != instance.transform.translation;

        SpatialEntry entry;
        entry.bounds = world_bounds_of(instance.local_bounds, placement);
        entry.stable_id = instance.stable_id;
        entry.layer_mask = instance.layer_mask;
        entry.flags = spatial_flags_of(instance.flags, moved);
        entry.domain = SpatialDomain::Renderable;
        entry.importance = instance.importance;
        entry.lod_bias = instance.lod_bias;

        if (Entry* existing = find(instance.stable_id); existing != nullptr) {
            if (Status moved_bounds = index_.update(existing->slot, entry.bounds); !moved_bounds) {
                return moved_bounds;
            }
            if (Status flagged = index_.set_flags(existing->slot, entry.flags); !flagged) {
                return flagged;
            }
            if (Status masked = index_.set_layer_mask(existing->slot, entry.layer_mask); !masked) {
                return masked;
            }
            existing->surface = Surface{instance.mesh, instance.material};
            out.updated += 1;
            continue;
        }

        const Expected<u32, Error> slot = index_.insert(entry);
        if (!slot) {
            return make_unexpected(slot.error());
        }
        if (slot_to_entry_.size() <= *slot) {
            const usize was = slot_to_entry_.size();
            if (Status sized = slot_to_entry_.resize(*slot + 1U); !sized) {
                return sized;
            }
            for (usize index = was; index < slot_to_entry_.size(); ++index) {
                slot_to_entry_[index] = kNoSlot;
            }
        }
        Entry record;
        record.stable_id = instance.stable_id;
        record.slot = *slot;
        record.surface = Surface{instance.mesh, instance.material};
        if (Status added = entries_.push_back(record); !added) {
            return added;
        }
        slot_to_entry_[*slot] = static_cast<u32>(entries_.size() - 1U);
        out.inserted += 1;
        live_ += 1;
    }

    for (const u64 stable_id : snapshot.removed) {
        Entry* existing = find(stable_id);
        if (existing == nullptr) {
            out.unknown_removals += 1;
            continue;
        }
        const u32 slot = existing->slot;
        if (Status gone = index_.remove(slot); !gone) {
            return gone;
        }
        // The entry is TOMBSTONED rather than erased: every other entry's position is a value in
        // `slot_to_entry_`, and compacting would rewrite all of them for one removal.
        existing->slot = kNoSlot;
        if (slot < slot_to_entry_.size()) {
            slot_to_entry_[slot] = kNoSlot;
        }
        out.removed += 1;
        live_ -= 1;
    }
    return ok();
}

}  // namespace cy::rendering::assembly
