#pragma once
// `VirtualTextureSystem` — the address spaces, the page tables, the shared caches, the feedback
// path, the producers and the prefetch, driven by the shared residency policy. Tasks 5.1 to 5.4.
//
// --- THE TWO GUARANTEES THIS FILE EXISTS TO KEEP
// ----------------------------------------------------
//
// M6's exit criterion for this capability is one sentence with two halves: **virtual texture
// feedback never blocks a frame, and the mip tail guarantees a frame is never missing.** They are
// kept in two different places and both are checkable:
//
//   * The first is `feedback.h`'s. `FeedbackBuffer::record` is wait-free and drops rather than
//     waits, and this system never calls anything on the recording path.
//   * The second is `sample()`. It walks from the level that was asked for towards the coarsest,
//     stopping at the first resident entry, and the mip tail is pinned in the cache and never
//     evictable — so as long as `make_mip_tail_resident` succeeded, the walk cannot fall off the
//     end. `SampleResult::missing` is therefore false for every address of a texture whose tail is
//     resident, which is a property a test asserts over the whole address space rather than a
//     sentence in a design document.
//
// --- WHY THIS SYSTEM HOLDS NO EVICTION POLICY
// -------------------------------------------------------
//
// `residency` owns scoring, budgets and eviction; this system owns storage and production. So the
// frame is: feedback becomes `residency::Request`s, the residency server answers with a
// `residency::Schedule`, and this system does what the schedule says. There is no method here that
// chooses which tile to drop, and `PhysicalTileCache::acquire` refuses rather than evicting so that
// no such method can grow by accident.
//
// --- TEARDOWN MID-FLIGHT, WHICH IS THE ORDER OF THE MEMBERS
// ------------------------------------------
//
// Production runs on worker threads that write into staging bytes the physical caches own. If the
// caches were destroyed before those threads were joined, a worker would be writing into freed
// memory — which is precisely the shape of the defect M5.5's gate found in Jolt's job bridge, one
// run in forty, as a SIGTRAP with nothing relevant on the stack.
//
// The fix is structural and is the last declaration in this class: `pool_` is declared LAST, so it
// is DESTROYED FIRST, so every worker is joined before a single cache, page table or staging buffer
// is touched. `unregister_texture()` and `reset()` call `pool_.quiesce()` for the same reason —
// they free storage a running job may hold a pointer into, and a comment saying "call this only
// when idle" is not a mechanism. Do not reorder the members.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/servers/render/virtual_texturing/address.h>
#include <cy/servers/render/virtual_texturing/feedback.h>
#include <cy/servers/render/virtual_texturing/page_table.h>
#include <cy/servers/render/virtual_texturing/physical_cache.h>
#include <cy/servers/render/virtual_texturing/producer.h>
#include <cy/servers/residency/deadline.h>
#include <cy/servers/residency/server.h>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace cy::render::vt {

/// The residency subsystem virtual texturing competes as. Named once here so that no call site
/// spells it, and a `PageKey` built anywhere in this module is a texture page by construction.
inline constexpr residency::Subsystem kResidencySubsystem = residency::Subsystem::Texture;

/// Turn an encoded virtual address into the residency layer's opaque page key. The 56-bit address
/// encoding was sized for exactly this; see `address.h`.
[[nodiscard]] inline residency::PageKey page_key(u64 encoded_address) noexcept {
    return residency::PageKey{kResidencySubsystem, encoded_address};
}

/// What a sample resolved to. `virtual-texturing`: "the difference between desired and resident
/// level SHALL be recorded for diagnostics and for request priority" — that is `deficit`, and it
/// reaches `residency::RequestInputs::detail_deficit` unchanged.
struct SampleResult {
    VirtualAddress resolved;
    u32 physical_tile = kNoPhysicalTile;
    u8 desired_mip = 0;
    u8 resident_mip = kNoResidentMip;
    u8 deficit = 0;
    bool fallback = false;
    /// Must be false for every address of a texture whose mip tail is resident. When it is true the
    /// tail was never made resident, which is a configuration error and not a streaming state.
    bool missing = true;
};

struct VirtualTextureStats {
    u64 textures = 0;
    u64 resident_tiles = 0;
    u64 pinned_tiles = 0;
    u64 cache_bytes = 0;
    u64 cache_budget = 0;

    u64 samples = 0;
    u64 fallback_samples = 0;
    u64 missing_samples = 0;

    u64 feedback_recorded = 0;
    u64 feedback_dropped = 0;
    u64 feedback_requests = 0;

    u64 requests_submitted = 0;
    u64 admissions_applied = 0;
    u64 admissions_refused = 0;
    u64 evictions_applied = 0;
    u64 pages_produced = 0;
    u64 production_failures = 0;
    u64 prefetched_pages = 0;

    u64 page_table_batches = 0;
    u64 page_table_updates = 0;

    /// `virtual-texturing`: "fallback sampling rate" is one of the numbers the profiler must show.
    [[nodiscard]] f64 fallback_rate() const noexcept {
        return (samples == 0) ? 0.0
                              : static_cast<f64>(fallback_samples) / static_cast<f64>(samples);
    }
};

/// One page's production work.
struct ProductionJob {
    ProductionRequest request;
    PageProducer* producer = nullptr;
};

/// What a worker finished.
struct ProductionResult {
    VirtualAddress address;
    u32 physical_tile = kNoPhysicalTile;
    bool succeeded = false;
};

/// The worker pool that runs producers off the frame's thread.
///
/// Its destructor stops and joins. `quiesce()` waits for the queue to drain AND for every running
/// job to finish, which is what makes it safe to free the storage a job holds a pointer into. Both
/// exist because M6 destroys worlds continuously; see the note at the top of this file.
class ProductionPool {
public:
    static constexpr u32 kMaxWorkers = 8;

    explicit ProductionPool(Allocator& allocator = current_allocator()) noexcept
        : queue_(allocator), done_(allocator) {}

    ProductionPool(const ProductionPool&) = delete;
    ProductionPool& operator=(const ProductionPool&) = delete;
    ProductionPool(ProductionPool&&) = delete;
    ProductionPool& operator=(ProductionPool&&) = delete;
    ~ProductionPool();

    Status start(u32 workers) noexcept;
    /// Idempotent. Sets the stop flag, wakes every worker and joins them.
    void stop() noexcept;
    /// Block until nothing is queued and nothing is running. Does NOT stop the workers.
    void quiesce() noexcept;

    Status submit(const ProductionJob& job) noexcept;
    /// Take the finished results. Returns how many were written.
    u32 drain(ProductionResult* out, u32 capacity) noexcept;

    /// How many workers are running. `start()` and `stop()` are the owner's calls and are not
    /// safe to make concurrently with each other; everything else on this class is.
    [[nodiscard]] u32 workers() const noexcept;
    [[nodiscard]] usize pending() const noexcept;
    [[nodiscard]] u64 produced() const noexcept;
    [[nodiscard]] u64 failed() const noexcept;

private:
    void worker_loop() noexcept;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    Array<ProductionJob> queue_;
    Array<ProductionResult> done_;
    std::thread threads_[kMaxWorkers];
    usize head_ = 0;
    u32 worker_count_ = 0;
    u32 running_ = 0;
    u64 produced_ = 0;
    u64 failed_ = 0;
    bool stopping_ = false;
};

class VirtualTextureSystem {
public:
    explicit VirtualTextureSystem(Allocator& allocator = current_allocator()) noexcept;
    ~VirtualTextureSystem();

    VirtualTextureSystem(const VirtualTextureSystem&) = delete;
    VirtualTextureSystem& operator=(const VirtualTextureSystem&) = delete;
    VirtualTextureSystem(VirtualTextureSystem&&) = delete;
    VirtualTextureSystem& operator=(VirtualTextureSystem&&) = delete;

    /// Configure one shared cache. One per format class; a second call for the same class replaces
    /// it, which drops every tile in it.
    Status configure_cache(const TileCacheDesc& desc) noexcept;
    [[nodiscard]] PhysicalTileCache& cache(FormatClass klass) noexcept;
    [[nodiscard]] const PhysicalTileCache& cache(FormatClass klass) const noexcept;

    Status start_production(u32 workers) noexcept;
    [[nodiscard]] u32 production_workers() const noexcept { return pool_.workers(); }

    Status register_texture(const VirtualTextureDesc& desc) noexcept;
    /// Quiesces production first — a running job may hold a pointer into this texture's tiles.
    bool unregister_texture(u32 texture) noexcept;
    [[nodiscard]] const VirtualTextureDesc* description(u32 texture) const noexcept;
    [[nodiscard]] const PageTable* page_table(u32 texture) const noexcept;
    [[nodiscard]] usize texture_count() const noexcept { return textures_.size(); }

    [[nodiscard]] FeedbackBuffer& feedback() noexcept { return feedback_; }
    [[nodiscard]] ProducerRegistry& producers() noexcept { return producers_; }

    /// Pin the coarsest levels of one texture into their cache and publish them in the page table.
    /// The whole of "a surface is never missing, only blurry", and it must succeed before any
    /// sample of this texture can be answered.
    Status make_mip_tail_resident(u32 texture) noexcept;
    [[nodiscard]] bool mip_tail_resident(u32 texture) const noexcept;

    /// Resolve one sample, walking to the nearest resident coarser level. Const and
    /// allocation-free.
    [[nodiscard]] SampleResult sample(const VirtualAddress& wanted) const noexcept;
    /// The same walk, plus the diagnostic counters. What a frame calls.
    SampleResult sample_and_count(const VirtualAddress& wanted) noexcept;

    /// Retire the frame's feedback, compact it, and submit one residency request per page.
    /// "A per-pixel request stream SHALL NOT reach the CPU" — what leaves here is one entry per
    /// page, whatever the sample count was.
    Status submit_feedback_requests(residency::ResidencyServer& server, f64 now) noexcept;

    /// `virtual-texturing` — "Predictive prefetch": request the COARSE pages of a texture ahead of
    /// need, against a deadline the residency layer propagated. "prediction covers latency,
    /// feedback establishes accuracy", so this asks for coarse levels only and never for the fine
    /// ones feedback will discover.
    Status prefetch(residency::ResidencyServer& server, u32 texture, u8 coarsest_wanted,
                    f64 seconds_until, residency::PredictionSource source, f64 now) noexcept;

    /// Act on the residency layer's decisions: take tiles for the admissions, dispatch production,
    /// release the tiles it ordered evicted.
    ///
    /// Takes the server because an admission this system cannot act on — its cache is full, its
    /// producer is missing — must be handed BACK. The policy spent budget on that page the moment
    /// it admitted it, and a system that silently declined would leave the bytes committed to a
    /// page that never arrives.
    Status apply(const residency::Schedule& schedule, residency::ResidencyServer& server,
                 f64 now) noexcept;

    /// Take the finished production, publish it in the page tables and tell residency the bytes
    /// are resident. Returns how many pages were published.
    u32 collect_production(residency::ResidencyServer& server, f64 now) noexcept;

    /// Apply every page table's staged updates — one batch per texture, never one call per page.
    void end_frame() noexcept;

    /// A producer's inputs changed. Invalidates the page and `finer_levels` below it, and releases
    /// their tiles so the pages are re-produced rather than served stale.
    Status invalidate(const VirtualAddress& address, u8 finer_levels) noexcept;

    /// Drop every texture, tile and pending job. Quiesces production first.
    void reset() noexcept;

    [[nodiscard]] VirtualTextureStats stats() const noexcept;

private:
    struct TextureRecord {
        VirtualTextureDesc desc;
        PageTable table;
        bool tail_resident = false;

        explicit TextureRecord(Allocator& allocator) noexcept : table(allocator) {}
    };

    [[nodiscard]] TextureRecord* find_texture(u32 texture) noexcept;
    [[nodiscard]] const TextureRecord* find_texture(u32 texture) const noexcept;
    [[nodiscard]] Status request_page(residency::ResidencyServer& server,
                                      const VirtualAddress& page, u32 samples, f32 confidence,
                                      f64 seconds_until, f64 now) noexcept;
    Status apply_admission(const residency::Admission& admission,
                           residency::ResidencyServer& server) noexcept;
    void apply_eviction(const residency::EvictionOrder& order) noexcept;
    void release_tile(const VirtualAddress& address) noexcept;

    Allocator* allocator_;
    Array<TextureRecord> textures_;
    HashMap<u64, usize> texture_slots_;
    // Same as `FeedbackBuffer::banks_`: the caches take the CONSTRUCTOR'S allocator, which a
    // default member initialiser cannot name.
    // NOLINTNEXTLINE(modernize-use-default-member-init)
    PhysicalTileCache caches_[kFormatClassCount];
    ProducerRegistry producers_;
    FeedbackBuffer feedback_;
    Array<FeedbackRequest> resolved_;
    VirtualTextureStats stats_;

    // DECLARED LAST SO IT IS DESTROYED FIRST. See the note at the top of this file. Moving this
    // line above `caches_` reintroduces a use-after-free that reproduces about one run in forty.
    ProductionPool pool_;
};

}  // namespace cy::render::vt
