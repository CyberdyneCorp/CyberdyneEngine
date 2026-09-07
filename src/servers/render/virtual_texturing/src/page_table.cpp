#include <cy/servers/render/virtual_texturing/page_table.h>

namespace cy::render::vt {

Status PageTable::configure(const VirtualTextureDesc& desc) noexcept {
    if (Status valid = validate_description(desc); !valid) {
        return valid;
    }
    clear();
    desc_ = desc;

    // Mip-major offsets, coarse levels last in memory but contiguous per level. Computed once so
    // that `flat_index` is an add rather than a loop over the pyramid.
    if (Status sized = mip_offsets_.resize(desc.mip_count); !sized) {
        return sized;
    }
    usize total = 0;
    for (u8 mip = 0; mip < desc.mip_count; ++mip) {
        mip_offsets_[mip] = total;
        total += static_cast<usize>(desc.tile_count(mip)) * desc.layers;
    }

    flat_representation_ = total <= kFlatEntryLimit;
    if (flat_representation_) {
        if (Status sized = flat_.resize(total); !sized) {
            return sized;
        }
        for (PageTableEntry& entry : flat_) {
            entry = PageTableEntry{};
        }
    }
    configured_ = true;
    return ok();
}

bool PageTable::in_range(const VirtualAddress& address) const noexcept {
    return configured_ && address.texture == desc_.id && address.mip < desc_.mip_count &&
           address.layer < desc_.layers && address.tile_x < desc_.tiles_x(address.mip) &&
           address.tile_y < desc_.tiles_y(address.mip);
}

usize PageTable::flat_index(const VirtualAddress& address) const noexcept {
    const auto per_layer = static_cast<usize>(desc_.tile_count(address.mip));
    const usize within =
        (static_cast<usize>(address.tile_y) * desc_.tiles_x(address.mip)) + address.tile_x;
    return mip_offsets_[address.mip] + (static_cast<usize>(address.layer) * per_layer) + within;
}

PageTableEntry PageTable::lookup(const VirtualAddress& address) const noexcept {
    if (!in_range(address)) {
        return PageTableEntry{};
    }
    if (flat_representation_) {
        return flat_[flat_index(address)];
    }
    const PageTableEntry* found = sparse_.find(address.encode());
    return (found == nullptr) ? PageTableEntry{} : *found;
}

Status PageTable::stage(const VirtualAddress& address, const PageTableEntry& entry) noexcept {
    if (!in_range(address)) {
        return make_unexpected(Error{ErrorCode::OutOfRange,
                                     "virtual texturing: staging an address outside the texture's "
                                     "declared address space"});
    }
    PageTableUpdate update;
    update.address = address.encode();
    update.entry = entry;
    // The generation is assigned here rather than by the caller: it is a property of how many times
    // the entry has been rewritten, and a caller that had to supply it would eventually not.
    update.entry.generation = static_cast<u16>(lookup(address).generation + 1U);
    return staged_.push_back(update);
}

void PageTable::write(const VirtualAddress& address, const PageTableEntry& entry) noexcept {
    if (flat_representation_) {
        flat_[flat_index(address)] = entry;
        return;
    }
    if (auto placed = sparse_.insert(address.encode(), entry); !placed) {
        // A sparse table that cannot grow drops the update. The page stays at whatever it was —
        // which, thanks to the mip tail, is a coarser level and not a missing one. Silently
        // succeeding with a lost entry would instead leave a physical tile nothing points at.
        return;
    }
}

usize PageTable::apply_staged() noexcept {
    for (const PageTableUpdate& update : staged_) {
        write(VirtualAddress::decode(update.address), update.entry);
    }
    last_batch_ = staged_.size();
    if (last_batch_ != 0) {
        ++batches_;
    }
    staged_.clear();
    return last_batch_;
}

Status PageTable::clear_page(const VirtualAddress& address) noexcept {
    // A PINNED ENTRY IS NOT A PRODUCER'S TO INVALIDATE. The mip tail is pinned, and it is the whole
    // of "a surface is never missing, only blurry"; a terrain deformation that cleared it would
    // take the guarantee away as a side effect of changing one page. Anything the residency layer
    // has pinned is skipped for the same reason.
    if (!in_range(address) || lookup(address).pinned()) {
        return ok();
    }
    PageTableEntry cleared;
    cleared.flags = PageFlags::kInvalid;
    return stage(address, cleared);
}

Status PageTable::clear_footprint(const VirtualAddress& coarse, u8 finer) noexcept {
    // The `finer` level's pages covering `coarse`'s footprint: 4^(coarse.mip - finer) of them.
    const u32 scale = 1U << (coarse.mip - finer);
    const u32 first_x = static_cast<u32>(coarse.tile_x) * scale;
    const u32 first_y = static_cast<u32>(coarse.tile_y) * scale;
    for (u32 offset_y = 0; offset_y < scale; ++offset_y) {
        for (u32 offset_x = 0; offset_x < scale; ++offset_x) {
            VirtualAddress address = coarse;
            address.mip = finer;
            address.tile_x = static_cast<u16>(first_x + offset_x);
            address.tile_y = static_cast<u16>(first_y + offset_y);
            if (Status cleared = clear_page(address); !cleared) {
                return cleared;
            }
        }
    }
    return ok();
}

Status PageTable::invalidate(const VirtualAddress& address, u8 finer_levels) noexcept {
    if (!in_range(address)) {
        return make_unexpected(
            Error{ErrorCode::OutOfRange, "virtual texturing: invalidating an unaddressable page"});
    }
    if (Status cleared = clear_page(address); !cleared) {
        return cleared;
    }
    if (finer_levels == 0 || address.mip == 0) {
        return ok();
    }

    // A producer whose inputs changed invalidates the region, not the page: the finer levels of the
    // same footprint are derived from the same inputs and are now wrong too. Walking down rather
    // than up, because the coarser levels cover more than the changed region and throwing them away
    // would invalidate content that did not change.
    const u8 deepest =
        (finer_levels >= address.mip) ? 0U : static_cast<u8>(address.mip - finer_levels);
    for (u8 finer = static_cast<u8>(address.mip - 1);; --finer) {
        if (Status cleared = clear_footprint(address, finer); !cleared) {
            return cleared;
        }
        if (finer == deepest) {
            break;
        }
    }
    return ok();
}

usize PageTable::resident_entries() const noexcept {
    usize count = 0;
    if (flat_representation_) {
        for (const PageTableEntry& entry : flat_) {
            count += entry.resident() ? 1U : 0U;
        }
        return count;
    }
    for (const auto& entry : sparse_) {
        count += entry.value.resident() ? 1U : 0U;
    }
    return count;
}

void PageTable::clear() noexcept {
    flat_.clear();
    sparse_.clear();
    staged_.clear();
    mip_offsets_.clear();
    last_batch_ = 0;
    batches_ = 0;
    flat_representation_ = false;
    configured_ = false;
}

}  // namespace cy::render::vt
