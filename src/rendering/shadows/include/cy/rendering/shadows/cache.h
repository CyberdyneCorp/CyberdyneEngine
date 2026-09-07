#pragma once
// The physical shadow page cache: residency, dirty state, age, eviction, and the record of why any
// of it happened. Tasks 8.1 and 8.2.
//
// `virtual-shadows` — "Shadow page cache" and "Shadow diagnostics".
//
// ================================================================================================
// RE-RENDERING A PAGE THAT WAS NOT INVALIDATED IS A DEFECT, AND THE STATISTICS SAY SO
// ================================================================================================
//
// "Caching SHALL be the default. Re-rendering a page that has not been invalidated SHALL be treated
// as a defect." So `request()` answers `needs_render` false for a resident, clean page, and
// `ShadowCacheStatistics::redundant_renders` counts every time someone asked for a render of a page
// that was already valid. A static scene viewed from a static camera renders its pages on the first
// frame and then reports zero renders forever, which is the first case in `test_cache.cpp`.
//
// ================================================================================================
// EVICTION IS WEIGHTED BY WHAT A PAGE COST TO PRODUCE
// ================================================================================================
//
// "evicted through the shared residency policy, weighted by the cost of re-rendering a page — an
// expensive static page is worth retaining longer than a cheap one." The victim score is therefore
// age divided by cost rather than age alone, and between two pages of equal recency the cheaper one
// goes. `min_residency_frames` — `residency`'s own field, mirrored here because this cache is a
// consumer of that policy rather than a second one — protects a page that has only just arrived,
// which is what stops two lights that both want the last slot evicting each other every frame.
//
// ================================================================================================
// EVERY DIRTY PAGE CARRIES WHY, BECAUSE "WHY IS THIS PAGE DIRTY" IS A REQUIRED DIAGNOSTIC
// ================================================================================================
//
// The specification's diagnostic scenario is literally "WHEN a page is re-rendered unexpectedly
// often THEN the diagnostics SHALL name what invalidated it". A dirty flag cannot answer that, so
// `PageEntry` carries an `InvalidationSource` and the id of whatever was responsible — an instance,
// a light, a world cell — and `inspect()` hands the whole entry back.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/rendering/shadows/address_space.h>
#include <cy/rendering/shadows/pages.h>

namespace cy::rendering {

/// What dirtied a page. Never absent on a dirty page: `None` is what a clean one carries.
enum class InvalidationSource : u8 {
    None = 0,
    /// A caster moved or deformed. `source_id` is the instance.
    Instance,
    /// The light itself moved. `source_id` is the light. Reported separately because "a moving
    /// shadowed light is expensive and the cost should be attributable rather than mysterious".
    LightMoved,
    /// World content streamed in or out. `source_id` is the cell.
    Streaming,
    /// The clipmap crossed a page boundary and this page is new.
    ClipmapShift,
    /// Somebody asked. `source_id` is the caller's own.
    Explicit,
    Count,
};

[[nodiscard]] const char* invalidation_source_name(InvalidationSource source) noexcept;

enum class PageState : u8 {
    /// The page has no physical slot.
    Absent = 0,
    /// A slot is allocated and holds a rendered result.
    Resident,
    /// A slot is allocated and a render is in flight into it. Its previous contents are still
    /// sampleable — which is what makes "a stale cached page" a rung of the fallback chain and not
    /// a black square.
    Rendering,
    Count,
};

[[nodiscard]] const char* page_state_name(PageState state) noexcept;

/// One entry of the page table. `virtual-shadows` enumerates what cache state per page must
/// include — "residency, dirty status, whether it is being rendered, its age, its update class, and
/// whether it is pinned" — and this is that list plus the attribution the diagnostics require.
struct PageEntry {
    VirtualPage page;
    u32 physical_slot = 0xFFFFFFFFU;
    PageState state = PageState::Absent;
    bool dirty = true;
    bool pinned = false;
    UpdateClass update_class = UpdateClass::Normal;
    /// Frame the page was last requested by a receiver.
    u64 last_used_frame = 0;
    /// Frame the page's contents were produced. Age is measured against it.
    u64 rendered_frame = 0;
    /// Milliseconds the last render of this page cost. Drives eviction weighting.
    f32 last_cost_ms = 0.0F;
    InvalidationSource dirty_reason = InvalidationSource::Explicit;
    /// The instance, light or cell named by `dirty_reason`.
    u64 dirty_source_id = 0;
};

/// What `request()` answered.
struct PageLookup {
    u32 physical_slot = 0xFFFFFFFFU;
    PageState state = PageState::Absent;
    /// True when the caller must rasterise into the slot this frame. False is the cache hit.
    bool needs_render = true;
    /// True when nothing could be allocated. The fallback chain takes over; the frame does not
    /// wait — "The frame SHALL never wait for shadow page production."
    bool starved = false;
};

struct ShadowCacheStatistics {
    u32 slots = 0;
    u32 slots_used = 0;
    u32 resident = 0;
    u32 requested = 0;
    u32 dirty = 0;
    u32 hits = 0;
    u32 renders = 0;
    u32 evictions = 0;
    /// Requests that could not be given a slot at all this frame.
    u32 starved = 0;
    /// Renders asked for on pages that were already clean and resident. Should be zero; a non-zero
    /// value is the defect the requirement names.
    u32 redundant_renders = 0;
    /// Pages dirtied this frame, by source. Indexed by `InvalidationSource`.
    u32 invalidated_by[static_cast<usize>(InvalidationSource::Count)] = {};
};

struct ShadowCacheConfig {
    /// Physical pages. The cache is shared across every light and clip level, so this is one number
    /// for the whole renderer and not one per light.
    u32 slots = 512;
    /// Frames a newly resident page is protected from eviction. `residency`'s own field.
    u32 min_residency_frames = 2;
    /// What a page is assumed to have cost when nothing has measured one. Only used to weight
    /// eviction before the first measurement arrives.
    f32 default_cost_ms = 0.05F;
};

/// The cache. Not thread-safe: it is stepped once per frame on the frame thread, between page
/// marking and the shadow passes.
class ShadowPageCache {
public:
    explicit ShadowPageCache(Allocator& allocator) noexcept;

    [[nodiscard]] Status initialize(const ShadowCacheConfig& config) noexcept;

    void begin_frame(u64 frame_index) noexcept;

    /// Ask for a page. Allocates a slot if the page has none, evicting if it must.
    [[nodiscard]] PageLookup request(VirtualPage page, UpdateClass update_class) noexcept;

    /// Record that a page was rasterised, and what it cost. Clears its dirty flag.
    void record_render(VirtualPage page, f32 cost_ms) noexcept;

    /// Mark a page dirty, naming what did it. Returns whether the cache holds the page at all: a
    /// page with no physical slot has no contents to invalidate, and counting it would inflate
    /// every invalidation report by the size of the light's address space.
    ///
    /// A page already dirty keeps its FIRST reason: the first thing to invalidate a page is the
    /// thing that caused the render, and overwriting it with the last would make the diagnostic
    /// name whatever happened to run last. The event is still counted.
    bool invalidate(VirtualPage page, InvalidationSource source, u64 source_id) noexcept;

    /// Pin or unpin a page. A pinned page is never evicted — the mip tail's guarantee, applied to
    /// shadows: see `fallback.h` for why a guaranteed coarse level is what stops the frame waiting.
    void set_pinned(VirtualPage page, bool pinned) noexcept;

    /// The whole entry, for the "why is this page dirty" diagnostic. Null when the cache has never
    /// seen the page.
    [[nodiscard]] const PageEntry* inspect(VirtualPage page) const noexcept;

    /// Every entry the cache holds, in slot order. Deterministic — the diagnostic overlays iterate
    /// it, and an order that depended on a hash seed would make a debug view flicker.
    [[nodiscard]] Span<const PageEntry> entries() const noexcept { return entries_.span(); }

    [[nodiscard]] const ShadowCacheStatistics& statistics() const noexcept { return stats_; }

private:
    [[nodiscard]] i32 find_slot(VirtualPage page) const noexcept;
    [[nodiscard]] i32 acquire_slot(VirtualPage page) noexcept;
    [[nodiscard]] i32 choose_victim() const noexcept;

    Array<PageEntry> entries_;
    HashMap<u64, u32> index_;
    ShadowCacheConfig config_;
    ShadowCacheStatistics stats_;
    u64 frame_ = 0;
};

}  // namespace cy::rendering
