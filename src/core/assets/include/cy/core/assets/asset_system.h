#ifndef CY_CORE_ASSETS_ASSET_SYSTEM_H
#define CY_CORE_ASSETS_ASSET_SYSTEM_H
// Asynchronous loading over the job system. Task 3.3.4.
//
// `core-assets-and-io` — "Asset loading": `load<T>(id)`, `load_async<T>(id)` returning a pollable
// request, `load_batch(ids)`, `preload`/`release`, reference counting through `Ref<T>` with a
// configurable retention policy; and **loads execute on the asset I/O thread and job workers: read,
// decompress, deserialize and upload are separate stages that can overlap across requests**.
//
// THE RULE THAT SHAPES EVERYTHING HERE: A WORKER NEVER BLOCKS.
// `core-jobs-and-concurrency` states it without qualification and enforces it — a blocking region
// declared on a thread executing a job is REFUSED and counted. So the stages are split by where
// they are allowed to run:
//
//   read          cy::jobs::AsyncService — the one thread where blocking is legal. It opens the
//                 file, seeks and reads the STORED bytes, still compressed.
//   decompress    a job. CPU work, on a worker, gated on the read's handle.
//   deserialize   a job, in the same body as decompress: the two are one continuation and
//                 splitting them further would buy nothing but two more scheduling hops.
//   upload        NOT HERE. It needs a renderer, which is M3. The stage is counted and skipped, and
//                 `AssetSystemStats::uploads_skipped` says so rather than the code pretending.
//
// Nothing in this file calls a blocking filesystem function from a job body. The one that reads is
// handed to `AsyncService::submit_blocking`, and `tests/test_asset_system.cpp` asserts
// `cy::jobs::blocking_violations()` is unchanged across a full load — a counter that is compiled
// into every configuration, so the assertion is not silently vacuous in Profile and Shipping.
//
// COALESCING, DEPENDENCIES AND CANCELLATION are the three behaviours the specification's scenarios
// name, and each is a property of the slot table rather than of a caller's discipline:
//   * two requests for one asset find the same slot, so ONE load runs and both get the same `Ref`;
//   * an entry's dependencies are read from the package directory and loaded before the request
//     reports completion, so a material's three textures are resident when it is;
//   * a cancelled request stops at the next STAGE boundary and releases partial results — each
//     stage begins by asking its token, which is the only place a stage can stop consistently.
//
// STREAMING IS M6. There is no residency budget driven by renderer feedback, no per-mip request and
// no priority derived from distance here. What is here is the retention policy the loading
// requirement itself names, which is about when a released asset's memory goes back — not about
// what fraction of an asset is resident.

#include <cy/core/assets/identity.h>
#include <cy/core/assets/package.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/jobs/async.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/ownership.h>

namespace cy::assets {

namespace detail {
struct AssetSystemImpl;
}

/// Where a load has got to. Also what a cancelled or failed one stopped at.
enum class LoadStage : u8 {
    Queued = 0,
    Reading = 1,
    Decompressing = 2,
    Deserializing = 3,
    /// Reserved for the GPU upload stage, which arrives with the renderer at M3.
    Uploading = 4,
    Complete = 5,
    Failed = 6,
    Cancelled = 7,
};

const char* load_stage_name(LoadStage stage) noexcept;

/// When the memory behind a released asset actually goes back.
enum class RetentionPolicy : u8 {
    /// The last `Ref` drops it at once. The least memory, the most reloading.
    Immediate = 0,
    /// Kept for `retention_delay_ns` after the last `Ref`, so a level transition that releases and
    /// immediately re-requests does not pay for a reload. `update()` is what collects it.
    TimeDelayed = 1,
    /// Kept until the resident total exceeds `residency_budget_bytes`, then the least recently used
    /// are dropped until it does not. `update()` is what evicts.
    BudgetBased = 2,
};

struct AssetSystemConfig {
    RetentionPolicy retention = RetentionPolicy::Immediate;
    /// For TimeDelayed. Ignored otherwise.
    i64 retention_delay_ns = 2'000'000'000;  // 2 s
    /// For BudgetBased. Ignored otherwise.
    usize residency_budget_bytes = usize{256} * 1024 * 1024;
    /// Check every payload against its recorded content hash. Off by default; a shipping build
    /// serving untrusted content turns it on, at the cost of hashing every byte it loads.
    bool verify_content_hashes = false;
    /// Loads in flight at once. Fixed, because growing the table while a worker is publishing into
    /// it would be an allocation on the completion path.
    u32 max_in_flight = 1024;
};

/// A loaded asset: cooked bytes, immutable after load.
///
/// `core-memory-and-containers` says exactly what `Ref<T>` is for — "shared ownership of
/// immutable-after-load data" — and this is the engine's first instance of it. Several workers read
/// one loaded asset with no synchronisation at all, because nothing writes it.
class AssetData : public RefCounted {
public:
    AssetData() noexcept = default;

    [[nodiscard]] cy::AssetId id() const noexcept { return id_; }
    [[nodiscard]] AssetKind kind() const noexcept { return kind_; }
    [[nodiscard]] VariantKey variant() const noexcept { return variant_; }

    /// The cooked payload. Points either into this object's own storage or into the package's
    /// memory mapping, and either way it outlives every `Ref` to it.
    [[nodiscard]] Span<const u8> bytes() const noexcept { return bytes_; }
    [[nodiscard]] usize size() const noexcept { return bytes_.size(); }

    /// True when this is a typed placeholder standing in for content that is not there.
    [[nodiscard]] bool is_placeholder() const noexcept { return placeholder_; }
    /// True when the payload is a view into a memory mapping rather than a copy.
    [[nodiscard]] bool is_mapped() const noexcept { return mapped_; }

private:
    friend struct detail::AssetSystemImpl;

    /// The system that published this asset, so the last release can tell it. A raw pointer and not
    /// a reference because an AssetData is constructed before it is adopted.
    detail::AssetSystemImpl* owner_ = nullptr;
    /// Which residency slot this is, so the last release finds it without a search.
    u32 slot_ = 0;

    cy::AssetId id_;
    VariantKey variant_;
    AssetKind kind_ = AssetKind::Unknown;
    bool placeholder_ = false;
    bool mapped_ = false;
    Array<u8> storage_;
    Span<const u8> bytes_;
};

using LoadRequestId = u64;
inline constexpr LoadRequestId kInvalidRequest = 0;

/// What a poll of a request reports.
struct LoadProgress {
    LoadStage stage = LoadStage::Queued;
    /// Assets in this request that have finished, and how many it asked for. A batch's progress.
    u32 completed = 0;
    u32 total = 0;
    bool done = false;
    bool failed = false;
    bool cancelled = false;
};

/// Per-load options. Both fields exist for one scenario: a reference whose asset is gone yields a
/// TYPED placeholder and a diagnostic naming the referrer, and neither the type nor the referrer is
/// knowable from the missing id.
struct LoadOptions {
    VariantKey variant;
    /// What the referrer expected. Chooses which placeholder is served when the asset is missing.
    AssetKind expected_kind = AssetKind::Unknown;
    /// Who is asking. Named in the diagnostic; nil when nobody in particular is.
    cy::AssetId referrer;
    jobs::Priority priority = jobs::Priority::Normal;
};

struct AssetSystemStats {
    u64 loads_started = 0;
    u64 loads_completed = 0;
    u64 loads_failed = 0;
    u64 loads_cancelled = 0;
    /// Requests that found a load already in flight or already resident, and rode it.
    u64 loads_coalesced = 0;
    u64 dependency_loads = 0;
    u64 bytes_read = 0;
    u64 bytes_decompressed = 0;
    /// Payloads served straight out of a memory mapping rather than copied.
    u64 entries_mapped = 0;
    u64 placeholders_served = 0;
    u64 integrity_failures = 0;
    u64 evictions = 0;
    /// The GPU upload stage, counted and skipped. See the note at the top of this file.
    u64 uploads_skipped = 0;
    usize resident_assets = 0;
    usize resident_bytes = 0;
    usize requests_in_flight = 0;
};

/// The engine's asset system.
class AssetSystem {
public:
    AssetSystem() noexcept;
    ~AssetSystem();

    AssetSystem(const AssetSystem&) = delete;
    AssetSystem& operator=(const AssetSystem&) = delete;

    /// Attach to the running job system, the async service and the mounted namespace.
    ///
    /// None of the three is owned: `core-jobs-and-concurrency` says the engine creates exactly one
    /// job system, and a subsystem that made its own would be the second.
    [[nodiscard]] Status start(jobs::JobSystem& jobs, jobs::AsyncService& async,
                               VirtualFileSystem& files, const AssetSystemConfig& config) noexcept;

    /// Cancel everything in flight, drain it, and release every resident asset. Idempotent.
    void shutdown() noexcept;

    [[nodiscard]] bool is_running() const noexcept;

    // --- Loading ------------------------------------------------------------------------------

    /// Blocking load. Never blocks a WORKER: it waits on the job graph, and a waiting participant
    /// runs other ready tasks rather than sleeping, which is what makes this safe to call from
    /// inside a job as well as from the main thread.
    [[nodiscard]] Expected<Ref<AssetData>, Error> load(cy::AssetId id,
                                                       const LoadOptions& options = {}) noexcept;

    /// Start a load and return a pollable request.
    [[nodiscard]] Expected<LoadRequestId, Error> load_async(
        cy::AssetId id, const LoadOptions& options = {}) noexcept;

    /// One request for many assets, so a dependency graph loads coherently and one poll answers for
    /// the whole set.
    [[nodiscard]] Expected<LoadRequestId, Error> load_batch(
        Span<const cy::AssetId> ids, const LoadOptions& options = {}) noexcept;

    [[nodiscard]] LoadProgress progress(LoadRequestId request) const noexcept;
    [[nodiscard]] bool is_complete(LoadRequestId request) const noexcept;

    /// Wait for a request. Runs other ready tasks while it waits, like `load`.
    [[nodiscard]] Status wait(LoadRequestId request) noexcept;

    /// The asset a single-id request loaded, or the first of a batch.
    [[nodiscard]] Expected<Ref<AssetData>, Error> result(LoadRequestId request) noexcept;
    /// One asset of a batch, by the index it was requested at.
    [[nodiscard]] Expected<Ref<AssetData>, Error> result_at(LoadRequestId request,
                                                            usize index) noexcept;

    /// Stop a pending request. It stops at the next STAGE boundary and releases partial results.
    /// A load another request is also waiting on is not cancelled — cancelling your own request
    /// must not cancel somebody else's.
    [[nodiscard]] Status cancel(LoadRequestId request) noexcept;

    /// Forget a completed request. Called by `update()` for requests nothing holds; a caller that
    /// polls rather than waits calls it when it has taken the result.
    [[nodiscard]] Status forget(LoadRequestId request) noexcept;

    // --- Residency ----------------------------------------------------------------------------

    /// Load and hold, without handing back a `Ref`. The explicit half of residency control.
    [[nodiscard]] Status preload(Span<const cy::AssetId> ids,
                                 const LoadOptions& options = {}) noexcept;

    /// Drop the hold `preload` took. Whether the memory goes back at once is the retention policy's
    /// answer, not this call's.
    [[nodiscard]] Status release(Span<const cy::AssetId> ids,
                                 const LoadOptions& options = {}) noexcept;

    /// The asset if it is already resident, without starting a load.
    [[nodiscard]] Expected<Ref<AssetData>, Error> find_resident(cy::AssetId id,
                                                                VariantKey variant = {}) noexcept;

    /// Retire finished requests, collect time-delayed releases, and evict against the budget.
    /// Called once a frame; a headless tool that never calls it keeps everything resident.
    void update() noexcept;

    [[nodiscard]] AssetSystemStats stats() const noexcept;
    void reset_stats() noexcept;

private:
    UniquePtr<detail::AssetSystemImpl> impl_;
};

}  // namespace cy::assets

#endif  // CY_CORE_ASSETS_ASSET_SYSTEM_H
