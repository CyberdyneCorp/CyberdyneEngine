#include <cy/servers/render/virtual_texturing/physical_cache.h>

namespace cy::render::vt {

Status PhysicalTileCache::configure(const TileCacheDesc& desc) noexcept {
    if (desc.tile_capacity == 0) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "virtual texturing: a physical cache with no slots"});
    }
    if (desc.bytes_per_tile == 0) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "virtual texturing: a cache must know its tile size in bytes, "
                                     "or the memory budget tree cannot apportion it"});
    }
    clear();
    desc_ = desc;

    if (Status sized = slots_.resize(desc.tile_capacity); !sized) {
        return sized;
    }
    for (TileSlot& slot : slots_) {
        slot = TileSlot{};
    }
    if (Status reserved = free_list_.reserve(desc.tile_capacity); !reserved) {
        return reserved;
    }
    // Highest index first, so that popping the back yields slot 0 outward. A cache filled from the
    // low end is one whose occupancy is legible in a dump.
    for (u32 index = desc.tile_capacity; index > 0; --index) {
        if (Status pushed = free_list_.push_back(index - 1); !pushed) {
            return pushed;
        }
    }
    if (Status sized =
            staging_.resize(static_cast<usize>(desc.tile_capacity) * desc.bytes_per_tile);
        !sized) {
        return sized;
    }
    configured_ = true;
    return ok();
}

Expected<u32, Error> PhysicalTileCache::acquire(u64 encoded_address, bool pinned) noexcept {
    if (!configured_) {
        return make_unexpected(
            Error{ErrorCode::Unavailable, "virtual texturing: cache used before configuration"});
    }
    if (const u32* existing = by_address_.find(encoded_address); existing != nullptr) {
        // Already here. Acquiring again is how a pin is added to a tile that arrived unpinned —
        // the mip tail is often already resident by the time it is made permanent.
        if (pinned && !slots_[*existing].pinned) {
            slots_[*existing].pinned = true;
            ++pinned_;
        }
        return *existing;
    }
    if (free_list_.empty()) {
        // REFUSED, NOT EVICTED. See the header note: choosing a victim is the shared residency
        // policy's, and a cache that chose one here would be a second policy.
        ++refusals_;
        return make_unexpected(Error{ErrorCode::Unavailable,
                                     "virtual texturing: the physical cache is full; the residency "
                                     "policy must release a tile before this one can be taken"});
    }

    const u32 tile = free_list_.back();
    free_list_.pop_back();
    if (auto placed = by_address_.insert(encoded_address, tile); !placed) {
        if (Status pushed = free_list_.push_back(tile); !pushed) {
            // The slot is lost to the free list but not to the cache: it is unoccupied, and a
            // later `clear()` recovers it. Failing the acquisition is the honest report.
        }
        return make_unexpected(placed.error());
    }

    slots_[tile].address = encoded_address;
    slots_[tile].occupied = true;
    slots_[tile].pinned = pinned;
    ++occupancy_;
    pinned_ += pinned ? 1U : 0U;
    ++acquisitions_;
    return tile;
}

bool PhysicalTileCache::release(u32 tile) noexcept {
    if (tile >= slots_.size() || !slots_[tile].occupied) {
        return false;
    }
    if (slots_[tile].pinned) {
        return false;  // a pin is a fact; unpin first, deliberately
    }
    by_address_.remove(slots_[tile].address);
    slots_[tile] = TileSlot{};
    --occupancy_;
    if (Status pushed = free_list_.push_back(tile); !pushed) {
        return true;  // released; the slot is simply not reusable until the next clear()
    }
    return true;
}

bool PhysicalTileCache::release_address(u64 encoded_address) noexcept {
    const u32* tile = by_address_.find(encoded_address);
    return (tile != nullptr) && release(*tile);
}

bool PhysicalTileCache::set_pinned(u32 tile, bool pinned) noexcept {
    if (tile >= slots_.size() || !slots_[tile].occupied) {
        return false;
    }
    if (slots_[tile].pinned == pinned) {
        return true;
    }
    slots_[tile].pinned = pinned;
    if (pinned) {
        ++pinned_;
    } else {
        --pinned_;
    }
    return true;
}

u32 PhysicalTileCache::find(u64 encoded_address) const noexcept {
    const u32* tile = by_address_.find(encoded_address);
    return (tile == nullptr) ? kNoPhysicalTile : *tile;
}

const TileSlot* PhysicalTileCache::slot(u32 tile) const noexcept {
    return (tile < slots_.size()) ? &slots_[tile] : nullptr;
}

u8* PhysicalTileCache::tile_data(u32 tile) noexcept {
    if (tile >= slots_.size() || staging_.empty()) {
        return nullptr;
    }
    return staging_.data() + (static_cast<usize>(tile) * desc_.bytes_per_tile);
}

const u8* PhysicalTileCache::tile_data(u32 tile) const noexcept {
    if (tile >= slots_.size() || staging_.empty()) {
        return nullptr;
    }
    return staging_.data() + (static_cast<usize>(tile) * desc_.bytes_per_tile);
}

void PhysicalTileCache::clear() noexcept {
    slots_.clear();
    free_list_.clear();
    by_address_.clear();
    staging_.clear();
    occupancy_ = 0;
    pinned_ = 0;
    configured_ = false;
}

}  // namespace cy::render::vt
