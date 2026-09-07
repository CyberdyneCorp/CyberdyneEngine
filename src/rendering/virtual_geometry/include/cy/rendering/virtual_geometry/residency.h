#pragma once
// The GPU geometry cache: one page table, one shared allocation, scored eviction, and the streaming
// feedback loop the GPU drives. M7 task 7.2.
//
// `virtual-geometry` — "GPU page table and residency": "The GPU SHALL address geometry through a
// **page table** mapping virtual page identifiers to locations in a **GPU geometry cache** of fixed
// budgeted size", entries carrying "a physical location, a generation counter, and state flags",
// the cache "a **single shared allocation**, not a per-asset buffer", and eviction "scored, not
// least-recently-used alone".
//
// ================================================================================================
// THE GENERATION COUNTER IS NOT BOOKKEEPING
// ================================================================================================
//
// "WHEN a page is evicted and its slot reused THEN the generation counter SHALL cause the old
// reference to miss rather than read the new page."
//
// A traversal reads the page table one frame and rasterises the next. In between, a page it saw as
// resident can be evicted and its slot filled with somebody else's geometry — and the byte offset
// it captured would then point at that geometry, which renders as a shard of the wrong mesh. The
// generation is what turns that into a miss: a reference carries the generation it was issued
// against, and a slot whose generation has moved fails the comparison. `test_residency.cpp` evicts
// underneath a held reference and asserts the miss.
//
// ================================================================================================
// WHY IT REGISTERS WITH cy::residency RATHER THAN HOLDING ITS OWN BUDGET
// ================================================================================================
//
// `residency` — "Geometry residency decisions SHALL remain the responsibility of this system; the
// shared layer supplies policy, priority, and budget, not storage." So the cache owns the bytes and
// the policy owns the decision to spend them: `GeometryCache` registers `Subsystem::Geometry`,
// reports every page it makes resident, and asks the server before admitting one. That is also what
// M7 task 4.3 needs — "more than one subsystem registered against the residency policy in a shipped
// path" — and virtual texturing is the other.
//
// A cache constructed WITHOUT a server is legal and is what a test of the cache alone uses: it then
// enforces its own budget and reports it. Said here rather than discovered.
//
// ================================================================================================
// WHAT MAY BE READ FROM ANOTHER THREAD, AND WHAT MAY NOT — STATED EXACTLY, BECAUSE TSAN CHECKS IT
// ================================================================================================
//
// `service()`, `prefetch()` and `redeem()` belong to the thread that owns the frame.
//
// `lookup()` and `resident()` MAY be called from another thread while that thread is servicing, and
// that is not a wish: the page table's fields are read and written through `std::atomic_ref` at
// relaxed ordering, so a concurrent read is defined rather than a data race. `test_residency.cpp`
// runs a reader beside a servicing owner under ThreadSanitizer, which is what turned the first
// version of this sentence from a claim into a fact — TSan reported twelve races against it.
//
// WHAT A CONCURRENT READER IS *NOT* PROMISED, and this is the part that matters:
//
//   * The four fields are individually atomic and the RECORD is not. A reader can see a new
//     `location` beside an old `generation`. That is why nothing acts on a concurrently-read entry:
//     the shipped path reads the table on the frame thread and uploads it, and the GPU's own
//     consistency comes from the render graph's barriers rather than from this.
//   * `register_asset()` and `unregister_asset()` RESIZE the table, which reallocates it. A reader
//     running across one of those reads freed memory. They are the owner's, like `service()`, and
//     unlike `service()` they may not overlap a reader at all.
//
// So the useful concurrent read is "is this page there, roughly, right now" — a diagnostic, a
// progress bar, a worker deciding whether to bother — and that is what it is for.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/rendering/virtual_geometry/asset.h>
#include <cy/rendering/virtual_geometry/traversal.h>
#include <cy/servers/residency/server.h>

namespace cy::rendering::vg {

/// `virtual-geometry`'s five page states. A bitmask because a page is legitimately several at once
/// — resident and pinned, requested and loading.
struct PageFlags {
    static constexpr u8 kInvalid = 0x01;    // nothing has been resolved for this page
    static constexpr u8 kResident = 0x02;   // the cache holds this page's bytes
    static constexpr u8 kRequested = 0x04;  // traversal asked for it; nothing is in flight yet
    static constexpr u8 kLoading = 0x08;    // admitted and being filled
    static constexpr u8 kPinned = 0x10;     // the always-resident root, or held by the policy
};

/// One entry of the GPU page table. Sixteen bytes, because the shader reads one per cluster per
/// frame and the table is as large as the asset set.
struct PageTableEntry {
    /// Byte offset into the shared cache allocation. `kNoLocation` when the page is not resident.
    u32 location = kNoLocation;
    u32 bytes = 0;
    /// Bumped every time the slot is reused. See the header comment: this is what makes a stale
    /// reference miss rather than read somebody else's geometry.
    u16 generation = 0;
    u8 flags = PageFlags::kInvalid;
    u8 reserved = 0;

    static constexpr u32 kNoLocation = 0xFFFFFFFFU;

    [[nodiscard]] constexpr bool resident() const noexcept {
        return (flags & PageFlags::kResident) != 0;
    }
    [[nodiscard]] constexpr bool pinned() const noexcept {
        return (flags & PageFlags::kPinned) != 0;
    }
};
static_assert(sizeof(PageTableEntry) == 12, "PageTableEntry must match cy/vg/records.slang");

/// Read one entry's fields at relaxed ordering, so that a reader on another thread is defined
/// rather than racing. Field by field: see the header comment for why the record as a whole is not
/// atomic and why nothing acts on a concurrently-read one.
[[nodiscard]] PageTableEntry load_entry(const PageTableEntry& entry) noexcept;

/// Write one entry's fields at relaxed ordering. The owner's call; the pairing with `load_entry` is
/// what makes a concurrent read defined.
void store_entry(PageTableEntry& entry, const PageTableEntry& value) noexcept;

/// A reference to a resident page, as a traversal captures it and a later pass redeems it.
struct PageReference {
    u32 asset = 0;
    u32 page = 0;
    u32 location = PageTableEntry::kNoLocation;
    u16 generation = 0;
};

/// What the cache is doing, for the profiler and for the causal questions the requirement asks it
/// to answer.
struct CacheStatistics {
    u64 budget_bytes = 0;
    u64 resident_bytes = 0;
    u32 resident_pages = 0;
    u32 pinned_pages = 0;
    u64 admissions = 0;
    u64 evictions = 0;
    u64 hits = 0;
    u64 misses = 0;
    /// Pages evicted and asked for again inside the churn window. `residency`'s own measure: "a
    /// cache can hold a 95% hit rate while thrashing the other 5%".
    u64 refetches = 0;
    /// Requests the streaming budget could not service this frame. "WHEN more pages are requested
    /// than the streaming budget allows THEN requests SHALL be prioritised and the remainder
    /// deferred, with the shortfall reported."
    u32 deferred_requests = 0;
    u64 stale_references = 0;

    [[nodiscard]] f64 hit_rate() const noexcept {
        const u64 total = hits + misses;
        return total == 0 ? 0.0 : static_cast<f64>(hits) / static_cast<f64>(total);
    }
    [[nodiscard]] f64 occupancy() const noexcept {
        return budget_bytes == 0
                   ? 0.0
                   : static_cast<f64>(resident_bytes) / static_cast<f64>(budget_bytes);
    }
};

struct CacheOptions {
    /// Bytes of the single shared allocation. `virtual-geometry`: "one shared cache within its
    /// budget, not per-asset allocations".
    u64 budget_bytes = 64ULL * 1024 * 1024;
    /// Pages admitted per service call. The streaming budget: the shortfall above it is deferred
    /// and reported rather than dropped.
    u32 admissions_per_frame = 64;
    /// Frames a page must have been resident before it may be evicted. `residency`'s minimum
    /// residency age, restated here because the cache evicts and the policy only scores.
    u32 minimum_residency_frames = 2;
};

/// The GPU geometry cache and its page table.
class GeometryCache {
public:
    explicit GeometryCache(const CacheOptions& options,
                           Allocator& allocator = current_allocator()) noexcept;

    GeometryCache(const GeometryCache&) = delete;
    GeometryCache& operator=(const GeometryCache&) = delete;
    GeometryCache(GeometryCache&&) = delete;
    GeometryCache& operator=(GeometryCache&&) = delete;
    ~GeometryCache();

    /// Register with the shared residency policy. Optional: a cache with no server enforces its own
    /// budget. `residency` supplies "policy, priority, and budget, not storage".
    [[nodiscard]] Status attach(residency::ResidencyServer& server) noexcept;

    /// Make an asset's pages addressable. The asset's always-resident pages are admitted and PINNED
    /// immediately, which is what makes "an object SHALL never fail to render because streaming has
    /// not completed" true from the first frame rather than after the first service.
    [[nodiscard]] Status register_asset(u32 asset, const DecodedAsset& description) noexcept;
    [[nodiscard]] Status unregister_asset(u32 asset) noexcept;

    /// The page table entry, or an invalid one. Never fails and never blocks, and safe to call from
    /// another thread while this one is servicing — see the header comment for what that does and
    /// does not promise.
    [[nodiscard]] PageTableEntry lookup(u32 asset, u32 page) const noexcept;
    [[nodiscard]] bool resident(u32 asset, u32 page) const noexcept;

    /// Redeem a reference captured on an earlier frame. Fails when the slot's generation has moved,
    /// which is the eviction the reference did not see.
    [[nodiscard]] Expected<u32, Error> redeem(const PageReference& reference) noexcept;

    /// Service one frame's requests. Scores them, admits what the streaming budget allows, evicts
    /// to make room, and reports the shortfall.
    ///
    /// `requests` is the compacted, deduplicated list traversal produced — the CPU reading the
    /// PREVIOUS frame's buffer, which is what "without a synchronising readback" means at this end.
    [[nodiscard]] Status service(Span<const PageRequest> requests, f64 now_seconds) noexcept;

    /// `virtual-geometry` — "Predictive streaming": prefetch from camera motion and cell activation
    /// rather than reacting alone. A prefetch is a request at a discount, so a reactive request for
    /// a page a frame needs now always outbids a prediction about one it may need later.
    [[nodiscard]] Status prefetch(u32 asset, Span<const u32> pages, f32 priority) noexcept;

    void set_budget_bytes(u64 bytes) noexcept;
    [[nodiscard]] u64 budget_bytes() const noexcept { return options_.budget_bytes; }
    [[nodiscard]] const CacheStatistics& statistics() const noexcept { return stats_; }
    [[nodiscard]] u64 frame() const noexcept { return frame_; }

    /// `virtual-geometry` — "Cache thrashing is diagnosable". The pages evicted and re-requested
    /// inside the window, worst first, with the asset each belongs to.
    [[nodiscard]] Status churning_pages(Array<PageRequest>& out) const noexcept;

    /// The whole table, for upload. One entry per page of every registered asset, indexed by the
    /// asset's page base plus the page index — which is what `page_base()` answers.
    [[nodiscard]] Span<const PageTableEntry> table() const noexcept { return entries_.span(); }
    [[nodiscard]] Expected<u32, Error> page_base(u32 asset) const noexcept;

private:
    struct AssetPages {
        u32 base = 0;
        u32 count = 0;
    };

    struct Resident {
        u32 entry = 0;
        u64 admitted_frame = 0;
        f64 last_used = 0.0;
        f32 priority = 0.0F;
    };

    [[nodiscard]] Status admit(u32 entry, u32 bytes, f32 priority, f64 now, bool pin) noexcept;
    [[nodiscard]] bool make_room(u64 bytes, f64 now) noexcept;
    void evict(usize slot) noexcept;
    [[nodiscard]] f32 score(const Resident& page, f64 now) const noexcept;
    void refresh_occupancy() noexcept;

    CacheOptions options_;
    Allocator& allocator_;
    HashMap<u32, AssetPages> assets_;
    Array<PageTableEntry> entries_;
    Array<u32> entry_bytes_;
    Array<Resident> resident_;
    /// Evicted pages and when, for the churn measure.
    HashMap<u32, f64> evicted_;
    residency::ResidencyServer* server_ = nullptr;
    CacheStatistics stats_;
    u64 frame_ = 0;
    u64 used_bytes_ = 0;
    u32 next_base_ = 0;
};

}  // namespace cy::rendering::vg
