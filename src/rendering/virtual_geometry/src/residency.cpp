#include <cy/rendering/virtual_geometry/residency.h>

#include <algorithm>
#include <atomic>

namespace cy::rendering::vg {

namespace {

/// The subsystem policy this cache registers. Every field is a statement `residency` asks a
/// subsystem to make about itself, and the two that are not defaults are the ones this capability
/// argues for: a geometry page is CHEAP to re-fetch — it is immutable, content-addressed bytes on
/// disk — and geometry reduces before textures do, because a coarser silhouette reads as further
/// away while a blurred texture reads as broken.
[[nodiscard]] residency::SubsystemPolicy geometry_policy(u64 budget_bytes) noexcept {
    residency::SubsystemPolicy policy;
    policy.domain = MemoryDomain::Gpu;
    policy.budget_bytes = budget_bytes;
    policy.budget_kind = BudgetKind::Hard;
    policy.reduction_order = 1;
    policy.default_cost = residency::CostClass::Streamed;
    policy.min_residency_frames = 2;
    // `residency` — "Reduction levers SHALL be declared per subsystem": the geometry error
    // threshold, in pixels, is this subsystem's whole lever. The three values are the pixel budget
    // at each pressure level, and the ladder is declared rather than computed so a reduction plan
    // can print it before it is applied.
    residency::LeverSchedule& threshold =
        policy.levers[static_cast<u32>(residency::Lever::GeometryErrorThreshold)];
    threshold.declared = true;
    threshold.normal = 1.0F;
    threshold.elevated = 2.0F;
    threshold.critical = 4.0F;
    return policy;
}

}  // namespace

PageTableEntry load_entry(const PageTableEntry& entry) noexcept {
    PageTableEntry copy;
    copy.location = std::atomic_ref<const u32>(entry.location).load(std::memory_order_relaxed);
    copy.bytes = std::atomic_ref<const u32>(entry.bytes).load(std::memory_order_relaxed);
    copy.generation = std::atomic_ref<const u16>(entry.generation).load(std::memory_order_relaxed);
    copy.flags = std::atomic_ref<const u8>(entry.flags).load(std::memory_order_relaxed);
    copy.reserved = std::atomic_ref<const u8>(entry.reserved).load(std::memory_order_relaxed);
    return copy;
}

void store_entry(PageTableEntry& entry, const PageTableEntry& value) noexcept {
    std::atomic_ref<u32>(entry.location).store(value.location, std::memory_order_relaxed);
    std::atomic_ref<u32>(entry.bytes).store(value.bytes, std::memory_order_relaxed);
    std::atomic_ref<u16>(entry.generation).store(value.generation, std::memory_order_relaxed);
    std::atomic_ref<u8>(entry.flags).store(value.flags, std::memory_order_relaxed);
    std::atomic_ref<u8>(entry.reserved).store(value.reserved, std::memory_order_relaxed);
}

GeometryCache::GeometryCache(const CacheOptions& options, Allocator& allocator) noexcept
    : options_(options),
      allocator_(allocator),
      assets_(allocator),
      entries_(allocator),
      entry_bytes_(allocator),
      resident_(allocator),
      evicted_(allocator) {
    stats_.budget_bytes = options.budget_bytes;
}

GeometryCache::~GeometryCache() = default;

Status GeometryCache::attach(residency::ResidencyServer& server) noexcept {
    if (Status registered = server.register_subsystem(residency::Subsystem::Geometry,
                                                      geometry_policy(options_.budget_bytes));
        !registered) {
        return registered;
    }
    server_ = &server;
    return ok();
}

Status GeometryCache::register_asset(u32 asset, const DecodedAsset& description) noexcept {
    if (assets_.find(asset) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "register_asset: that asset is already registered");
    }
    AssetPages record;
    record.base = next_base_;
    record.count = static_cast<u32>(description.pages.size());
    if (record.count == 0) {
        return fail(ErrorCode::InvalidArgument, "register_asset: the asset has no pages");
    }
    if (Status resized = entries_.resize(record.base + record.count); !resized) {
        return resized;
    }
    if (Status resized = entry_bytes_.resize(record.base + record.count); !resized) {
        return resized;
    }
    for (u32 index = 0; index < record.count; ++index) {
        entries_[record.base + index] = PageTableEntry{};
        entry_bytes_[record.base + index] = description.pages[index].byte_size;
    }
    Expected<AssetPages*, Error> inserted = assets_.insert(asset, record);
    if (!inserted) {
        return make_unexpected(inserted.error());
    }
    next_base_ += record.count;

    // THE ALWAYS-RESIDENT ROOT IS ADMITTED HERE AND PINNED, not on the first service. Waiting would
    // make "an object SHALL never fail to render because streaming has not completed" true from the
    // second frame, which is the frame after the one where it matters.
    for (u32 index = 0; index < record.count; ++index) {
        if (!description.pages[index].resident) {
            continue;
        }
        if (Status admitted = admit(record.base + index, description.pages[index].byte_size,
                                    3.402823466e+38F, 0.0, true);
            !admitted) {
            return admitted;
        }
    }
    return ok();
}

Status GeometryCache::unregister_asset(u32 asset) noexcept {
    const AssetPages* record = assets_.find(asset);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "unregister_asset: no such asset");
    }
    const u32 base = record->base;
    const u32 count = record->count;
    for (usize slot = resident_.size(); slot > 0; --slot) {
        const usize index = slot - 1;
        if (resident_[index].entry >= base && resident_[index].entry < base + count) {
            evict(index);
        }
    }
    for (u32 index = 0; index < count; ++index) {
        entries_[base + index] = PageTableEntry{};
        entry_bytes_[base + index] = 0;
    }
    (void)assets_.remove(asset);
    return ok();
}

Expected<u32, Error> GeometryCache::page_base(u32 asset) const noexcept {
    const AssetPages* record = assets_.find(asset);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "page_base: no such asset");
    }
    return record->base;
}

PageTableEntry GeometryCache::lookup(u32 asset, u32 page) const noexcept {
    const AssetPages* record = assets_.find(asset);
    if (record == nullptr || page >= record->count) {
        return PageTableEntry{};
    }
    return load_entry(entries_[record->base + page]);
}

bool GeometryCache::resident(u32 asset, u32 page) const noexcept {
    return lookup(asset, page).resident();
}

Expected<u32, Error> GeometryCache::redeem(const PageReference& reference) noexcept {
    const PageTableEntry entry = lookup(reference.asset, reference.page);
    if (!entry.resident() || entry.generation != reference.generation ||
        entry.location != reference.location) {
        // THE WHOLE POINT OF THE GENERATION. The slot may hold somebody else's geometry now, and
        // the captured offset would render it as a shard of the wrong mesh.
        ++stats_.stale_references;
        return fail(ErrorCode::Unavailable,
                    "redeem: the page was evicted after this reference was taken");
    }
    return entry.location;
}

f32 GeometryCache::score(const Resident& page, f64 now) const noexcept {
    const PageTableEntry& entry = entries_[page.entry];
    if (entry.pinned()) {
        return residency::kNeverEvict;
    }
    if (frame_ - page.admitted_frame < options_.minimum_residency_frames) {
        // `residency`: "A page SHALL have a **minimum residency age** before becoming evictable".
        // The other half of "no oscillation": a page that arrived this frame cannot leave this
        // frame, however badly it scores.
        return residency::kNeverEvict;
    }
    // `virtual-geometry` — "Eviction SHALL be scored, not least-recently-used alone: visibility,
    // projected area, recent usage, object importance, and predicted usage SHALL contribute." The
    // priority a request carried is the first four already combined — it is the cluster's screen
    // size scaled by its instance's importance — so what is left is the recency term.
    const f32 age = static_cast<f32>(now - page.last_used);
    const f32 recency = 1.0F / (1.0F + (age > 0.0F ? age : 0.0F));
    return (page.priority * 0.75F) + (recency * 0.25F);
}

bool GeometryCache::make_room(u64 bytes, f64 now) noexcept {
    if (used_bytes_ + bytes <= options_.budget_bytes) {
        return true;
    }
    while (used_bytes_ + bytes > options_.budget_bytes) {
        usize worst = resident_.size();
        f32 worst_score = residency::kNeverEvict;
        for (usize index = 0; index < resident_.size(); ++index) {
            const f32 candidate = score(resident_[index], now);
            if (candidate < worst_score) {
                worst_score = candidate;
                worst = index;
            }
        }
        if (worst == resident_.size()) {
            return false;  // everything is pinned or too young: the admission is refused
        }
        (void)evicted_.insert(resident_[worst].entry, now);
        evict(worst);
    }
    return true;
}

void GeometryCache::evict(usize slot) noexcept {
    const Resident page = resident_[slot];
    PageTableEntry& slot_entry = entries_[page.entry];
    used_bytes_ -= slot_entry.bytes;
    // The generation moves BEFORE the slot is reused, so a reference taken against the old
    // generation fails its comparison rather than reading whatever lands here next.
    PageTableEntry cleared;
    cleared.generation = static_cast<u16>(slot_entry.generation + 1U);
    cleared.location = PageTableEntry::kNoLocation;
    cleared.bytes = 0;
    cleared.flags = PageFlags::kInvalid;
    store_entry(slot_entry, cleared);
    resident_.remove_unordered(slot);
    ++stats_.evictions;
    refresh_occupancy();
    if (server_ != nullptr) {
        (void)server_->set_active(residency::PageKey{residency::Subsystem::Geometry, page.entry},
                                  false);
    }
}

Status GeometryCache::admit(u32 entry_index, u32 bytes, f32 priority, f64 now, bool pin) noexcept {
    PageTableEntry& entry = entries_[entry_index];
    if (entry.resident()) {
        PageTableEntry pinned = entry;
        pinned.flags = static_cast<u8>(entry.flags | (pin ? PageFlags::kPinned : 0U));
        store_entry(entry, pinned);
        return ok();
    }
    if (!make_room(bytes, now)) {
        ++stats_.deferred_requests;
        return fail(ErrorCode::OutOfMemory, "admit: the geometry cache has no room for this page");
    }
    // The cache is ONE shared allocation and the location is a byte offset into it. A suballocator
    // that packed by size would belong here; a bump over the accounted total is what M7 ships and
    // README.md says so, because the eviction policy is the part the requirement is about and a
    // suballocator would make the tests about fragmentation instead.
    PageTableEntry admitted = entry;
    admitted.location = static_cast<u32>(used_bytes_);
    admitted.bytes = bytes;
    admitted.flags = static_cast<u8>(PageFlags::kResident | (pin ? PageFlags::kPinned : 0U));
    store_entry(entry, admitted);
    used_bytes_ += bytes;

    Resident record;
    record.entry = entry_index;
    record.admitted_frame = frame_;
    record.last_used = now;
    record.priority = priority;
    if (Status pushed = resident_.push_back(record); !pushed) {
        return pushed;
    }
    ++stats_.admissions;
    refresh_occupancy();

    if (const f64* when = evicted_.find(entry_index); when != nullptr) {
        // Evicted and asked for again: churn, which "is invisible in hit-rate statistics alone".
        if (now - *when <= 1.0) {
            ++stats_.refetches;
        }
        (void)evicted_.remove(entry_index);
    }

    if (server_ != nullptr) {
        residency::ResidentReport report;
        report.key = residency::PageKey{residency::Subsystem::Geometry, entry_index};
        report.bytes = bytes;
        report.guaranteed = pin;
        report.cost = residency::CostClass::Streamed;
        (void)server_->note_resident(report, now);
        (void)server_->set_active(report.key, true);
    }
    return ok();
}

Status GeometryCache::service(Span<const PageRequest> requests, f64 now_seconds) noexcept {
    ++frame_;
    stats_.deferred_requests = 0;

    // Priority order, ties by (asset, page) so two runs of one frame admit the same pages.
    Array<PageRequest> ordered(allocator_);
    if (Status reserved = ordered.reserve(requests.size()); !reserved) {
        return reserved;
    }
    for (const PageRequest& request : requests) {
        if (Status pushed = ordered.push_back(request); !pushed) {
            return pushed;
        }
    }
    std::ranges::sort(ordered, [](const PageRequest& a, const PageRequest& b) noexcept {
        if (a.priority != b.priority) {
            return a.priority > b.priority;
        }
        if (a.asset != b.asset) {
            return a.asset < b.asset;
        }
        return a.page < b.page;
    });

    u32 admitted = 0;
    for (const PageRequest& request : ordered) {
        const AssetPages* record = assets_.find(request.asset);
        if (record == nullptr || request.page >= record->count) {
            continue;
        }
        const u32 entry_index = record->base + request.page;
        if (entries_[entry_index].resident()) {
            ++stats_.hits;
            for (Resident& page : resident_) {
                if (page.entry == entry_index) {
                    page.last_used = now_seconds;
                    page.priority =
                        request.priority > page.priority ? request.priority : page.priority;
                }
            }
            continue;
        }
        ++stats_.misses;
        if (admitted >= options_.admissions_per_frame) {
            // "WHEN more pages are requested than the streaming budget allows THEN requests SHALL
            // be prioritised and the remainder deferred, with the shortfall reported."
            ++stats_.deferred_requests;
            continue;
        }
        if (server_ != nullptr) {
            residency::Request policy_request;
            policy_request.key = residency::PageKey{residency::Subsystem::Geometry, entry_index};
            policy_request.bytes = entry_bytes_[entry_index];
            policy_request.inputs.screen_coverage = request.priority;
            policy_request.inputs.importance = request.priority;
            policy_request.inputs.prediction_confidence = 1.0F;
            (void)server_->request(policy_request);
        }
        if (Status ready =
                admit(entry_index, entry_bytes_[entry_index], request.priority, now_seconds, false);
            !ready) {
            continue;  // no room: already counted as deferred by admit()
        }
        ++admitted;
    }

    if (server_ != nullptr) {
        server_->end_frame(now_seconds);
    }
    return ok();
}

Status GeometryCache::prefetch(u32 asset, Span<const u32> pages, f32 priority) noexcept {
    Array<PageRequest> requests(allocator_);
    if (Status reserved = requests.reserve(pages.size()); !reserved) {
        return reserved;
    }
    for (const u32 page : pages) {
        // A PREDICTION IS A REQUEST AT A DISCOUNT. `virtual-geometry` asks for predictive streaming
        // beside the reactive path, and the two have to compete for one budget: a reactive request
        // is a measurement of what a frame needed, a prefetch is a guess about what a later one
        // will. Halving the priority is what makes the first always outbid the second for the same
        // pixels, without a second queue that would need its own budget.
        if (Status pushed = requests.push_back(PageRequest{asset, page, priority * 0.5F, 1U});
            !pushed) {
            return pushed;
        }
    }
    return service(requests.span(), static_cast<f64>(frame_));
}

/// The occupancy figures, refreshed wherever they change rather than once a frame.
///
/// They were once updated only at the end of `service()`, and the cost was a lie an asset
/// registration could tell: a cache whose always-resident roots were admitted at `register_asset`
/// reported zero resident bytes until a frame had been serviced, which is the report a diagnostic
/// reads to answer "why is this region consuming this much".
void GeometryCache::refresh_occupancy() noexcept {
    stats_.resident_bytes = used_bytes_;
    stats_.resident_pages = static_cast<u32>(resident_.size());
    stats_.pinned_pages = 0;
    for (const Resident& page : resident_) {
        stats_.pinned_pages += entries_[page.entry].pinned() ? 1U : 0U;
    }
}

void GeometryCache::set_budget_bytes(u64 bytes) noexcept {
    options_.budget_bytes = bytes;
    stats_.budget_bytes = bytes;
}

Status GeometryCache::churning_pages(Array<PageRequest>& out) const noexcept {
    for (const auto& entry : evicted_) {
        for (const auto& asset : assets_) {
            if (entry.key < asset.value.base || entry.key >= asset.value.base + asset.value.count) {
                continue;
            }
            if (Status pushed = out.push_back(PageRequest{asset.key, entry.key - asset.value.base,
                                                          static_cast<f32>(entry.value), 1U});
                !pushed) {
                return pushed;
            }
        }
    }
    std::ranges::sort(out, [](const PageRequest& a, const PageRequest& b) noexcept {
        if (a.asset != b.asset) {
            return a.asset < b.asset;
        }
        return a.page < b.page;
    });
    return ok();
}

}  // namespace cy::rendering::vg
