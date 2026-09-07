#include <cy/rendering/shadows/cache.h>

#include <cy/core/math/scalar.h>

namespace cy::rendering {

const char* invalidation_source_name(InvalidationSource source) noexcept {
    switch (source) {
        case InvalidationSource::None:
            return "None";
        case InvalidationSource::Instance:
            return "Instance";
        case InvalidationSource::LightMoved:
            return "LightMoved";
        case InvalidationSource::Streaming:
            return "Streaming";
        case InvalidationSource::ClipmapShift:
            return "ClipmapShift";
        case InvalidationSource::Explicit:
            return "Explicit";
        case InvalidationSource::Count:
            break;
    }
    return "Unknown";
}

const char* page_state_name(PageState state) noexcept {
    switch (state) {
        case PageState::Absent:
            return "Absent";
        case PageState::Resident:
            return "Resident";
        case PageState::Rendering:
            return "Rendering";
        case PageState::Count:
            break;
    }
    return "Unknown";
}

ShadowPageCache::ShadowPageCache(Allocator& allocator) noexcept
    : entries_(allocator), index_(allocator) {}

Status ShadowPageCache::initialize(const ShadowCacheConfig& config) noexcept {
    if (config.slots == 0) {
        return fail(ErrorCode::InvalidArgument, "shadow page cache: slots must be non-zero");
    }
    config_ = config;
    entries_.clear();
    if (Status reserved = entries_.reserve(config.slots); !reserved) {
        return reserved;
    }
    if (Status reserved = index_.reserve(config.slots); !reserved) {
        return reserved;
    }
    index_.clear();
    stats_ = ShadowCacheStatistics{};
    stats_.slots = config.slots;
    return ok();
}

void ShadowPageCache::begin_frame(u64 frame_index) noexcept {
    frame_ = frame_index;
    stats_.requested = 0;
    stats_.hits = 0;
    stats_.renders = 0;
    stats_.evictions = 0;
    stats_.starved = 0;
    stats_.redundant_renders = 0;
    for (u32& count : stats_.invalidated_by) {
        count = 0;
    }
    // Recomputed rather than maintained. Residency and dirtiness are read by the profiler once a
    // frame and changed many times within one, so a counter kept in step at every mutation is a
    // counter with more places to go wrong than the loop it replaces.
    stats_.slots_used = static_cast<u32>(entries_.size());
    stats_.resident = 0;
    stats_.dirty = 0;
    for (const PageEntry& entry : entries_.span()) {
        if (entry.state != PageState::Absent) {
            ++stats_.resident;
        }
        if (entry.dirty) {
            ++stats_.dirty;
        }
    }
}

i32 ShadowPageCache::find_slot(VirtualPage page) const noexcept {
    const u32* slot = index_.find(page.pack());
    return slot == nullptr ? -1 : static_cast<i32>(*slot);
}

i32 ShadowPageCache::choose_victim() const noexcept {
    // Age divided by cost: between two pages of equal recency the cheaper one goes, which is the
    // requirement's "an expensive static page is worth retaining longer than a cheap one". A page
    // used this frame, pinned, being rendered into, or younger than the minimum residency is not a
    // candidate at all — the last of those is what stops two lights thrashing over one slot.
    i32 victim = -1;
    f32 best = -1.0F;
    const usize count = entries_.size();
    for (usize index = 0; index < count; ++index) {
        const PageEntry& entry = entries_.span()[index];
        if (entry.pinned || entry.state == PageState::Rendering ||
            entry.last_used_frame == frame_) {
            continue;
        }
        if (frame_ < entry.rendered_frame + config_.min_residency_frames) {
            continue;
        }
        const f32 age = static_cast<f32>(frame_ - entry.last_used_frame);
        const f32 cost = math::max(entry.last_cost_ms, config_.default_cost_ms);
        const f32 score = age / cost;
        if (score > best) {
            best = score;
            victim = static_cast<i32>(index);
        }
    }
    return victim;
}

i32 ShadowPageCache::acquire_slot(VirtualPage page) noexcept {
    if (entries_.size() < config_.slots) {
        PageEntry entry;
        entry.page = page;
        entry.physical_slot = static_cast<u32>(entries_.size());
        if (Status pushed = entries_.push_back(entry); !pushed) {
            return -1;
        }
        const i32 slot = static_cast<i32>(entries_.size() - 1);
        if (Expected<u32*, Error> inserted = index_.insert(page.pack(), static_cast<u32>(slot));
            !inserted) {
            entries_.pop_back();
            return -1;
        }
        return slot;
    }

    const i32 victim = choose_victim();
    if (victim < 0) {
        return -1;
    }
    PageEntry& entry = entries_.span()[static_cast<usize>(victim)];
    (void)index_.remove(entry.page.pack());
    ++stats_.evictions;
    const u32 physical = entry.physical_slot;
    entry = PageEntry{};
    entry.page = page;
    entry.physical_slot = physical;
    if (Expected<u32*, Error> inserted = index_.insert(page.pack(), static_cast<u32>(victim));
        !inserted) {
        return -1;
    }
    return victim;
}

PageLookup ShadowPageCache::request(VirtualPage page, UpdateClass update_class) noexcept {
    ++stats_.requested;
    PageLookup lookup;
    i32 slot = find_slot(page);
    if (slot < 0) {
        slot = acquire_slot(page);
        if (slot < 0) {
            ++stats_.starved;
            lookup.starved = true;
            return lookup;
        }
    }
    PageEntry& entry = entries_.span()[static_cast<usize>(slot)];
    entry.last_used_frame = frame_;
    entry.update_class = update_class;
    lookup.physical_slot = entry.physical_slot;
    lookup.state = entry.state;
    lookup.needs_render = entry.dirty || entry.state == PageState::Absent;
    if (!lookup.needs_render) {
        ++stats_.hits;
    }
    return lookup;
}

void ShadowPageCache::record_render(VirtualPage page, f32 cost_ms) noexcept {
    const i32 slot = find_slot(page);
    if (slot < 0) {
        return;
    }
    PageEntry& entry = entries_.span()[static_cast<usize>(slot)];
    if (!entry.dirty && entry.state == PageState::Resident) {
        // The defect the requirement names, counted rather than tolerated.
        ++stats_.redundant_renders;
    }
    entry.state = PageState::Resident;
    entry.dirty = false;
    entry.dirty_reason = InvalidationSource::None;
    entry.dirty_source_id = 0;
    entry.rendered_frame = frame_;
    entry.last_cost_ms = cost_ms;
    ++stats_.renders;
}

bool ShadowPageCache::invalidate(VirtualPage page, InvalidationSource source,
                                 u64 source_id) noexcept {
    const i32 slot = find_slot(page);
    if (slot < 0) {
        return false;
    }
    PageEntry& entry = entries_.span()[static_cast<usize>(slot)];
    ++stats_.invalidated_by[static_cast<usize>(source)];
    if (entry.dirty) {
        // The FIRST reason is kept: the thing that caused the render is the thing that dirtied it,
        // and overwriting with the last would make the diagnostic name whatever ran most recently.
        return true;
    }
    entry.dirty = true;
    entry.dirty_reason = source;
    entry.dirty_source_id = source_id;
    return true;
}

void ShadowPageCache::set_pinned(VirtualPage page, bool pinned) noexcept {
    i32 slot = find_slot(page);
    if (slot < 0) {
        slot = acquire_slot(page);
        if (slot < 0) {
            return;
        }
    }
    entries_.span()[static_cast<usize>(slot)].pinned = pinned;
}

const PageEntry* ShadowPageCache::inspect(VirtualPage page) const noexcept {
    const i32 slot = find_slot(page);
    return slot < 0 ? nullptr : &entries_.span()[static_cast<usize>(slot)];
}

}  // namespace cy::rendering
