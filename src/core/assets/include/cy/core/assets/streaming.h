#ifndef CY_CORE_ASSETS_STREAMING_H
#define CY_CORE_ASSETS_STREAMING_H
// Partial residency: mip levels, mesh LODs and audio chunks, under a budget. M7 tasks 2.1 and 2.2.
//
// `core-assets-and-io` — "Streaming": "The engine SHALL support partial residency for assets that
// declare it — textures (mip levels), meshes (LOD levels), and audio (streamed samples) — driven by
// a residency budget and per-asset priority derived from renderer feedback and distance.
// Streaming SHALL never block the frame: a not-yet-resident level SHALL fall back to the highest
// resident level."
//
// M5 shipped whole-asset loading and M6 planned this requirement and did not touch it —
// `git diff --stat -- src/core/` was empty for that milestone, and `asset_system.h` still said
// "STREAMING IS M6. There is no residency budget". It lands here because M7 is the milestone whose
// virtual texturing and virtual geometry are the feedback that drives it.
//
// --- THE THREE DECISIONS THAT SHAPE THIS FILE ----------------------------------------------------
//
// **A LEVEL IS A BYTE RANGE, AND THE ASSET DECLARES IT.** Nothing here parses a texture header or a
// mesh. Layer 0 has no idea what a mip is; it knows that a cooked payload has an ordered ladder of
// ranges, that position 0 is the always-resident base, and that a range can be read on its own. The
// consumer that DOES understand the format — the texture loader, the virtual-geometry cook, the
// audio decoder — declares the ladder, exactly as `residency`'s `LeverSchedule` is declared by the
// subsystem that owns the lever rather than guessed at by the arbiter.
//
// **REQUESTING IS NOT LOADING, AND `resident_level` NEVER WAITS.** `request` records what the
// renderer asked for and returns; `update` decides what to admit within the budget and issues the
// reads on the async service, which is the one thread where blocking is legal. A frame that asks
// for mip 3 and gets mip 5 draws mip 5 — that IS the requirement's second sentence, and it is the
// reason `resident_level` is a plain lookup with no failure mode.
//
// **EVICTION IS LEAST-RECENTLY-REQUESTED, AND NEVER TAKES THE BASE.** The specification's own
// scenario: "WHEN the residency budget is exceeded THEN the least recently requested levels SHALL
// be evicted first, and the eviction SHALL be recorded in streaming statistics." Level 0 is
// exempt — an asset whose base was evicted has nothing to fall back TO, and the fallback is the
// whole guarantee.
//
// --- WHERE THE BUDGET COMES FROM -----------------------------------------------------------------
//
// A number, set by whoever owns the memory policy. `AssetSystem` sets it from
// `AssetSystemConfig::residency_budget_bytes` at `start()`, so streaming is on in every build
// without anybody opting in; `cy::servers::residency` re-sets it every time the arbiter
// reallocates. This module deliberately does not reach up to the residency server: layer 0 cannot,
// and a budget that arrives as a number is a budget any owner can drive.

#include <cy/core/assets/identity.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/jobs/async.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/ownership.h>
#include <cy/core/memory/system_allocator.h>

#include <atomic>

namespace cy::assets {

/// What kind of ladder an asset declared. A label: the mechanism is identical for all three, and
/// the statistics and a diagnostic are the reason it is recorded at all.
enum class StreamKind : u8 {
    /// Texture mip levels. Position 0 is the mip TAIL — the smallest levels, which is what makes a
    /// virtual texture's tail guarantee expressible here.
    TextureMip = 0,
    /// Mesh levels of detail. Position 0 is the coarsest.
    MeshLod = 1,
    /// Streamed audio. Position 0 is the first chunk, which is what a sound needs to start at all.
    AudioChunk = 2,
};

[[nodiscard]] const char* stream_kind_name(StreamKind kind) noexcept;

/// One position of an asset's ladder: where its bytes are within the asset's file, and how many.
///
/// Positions are declared coarsest-first and each one is INDEPENDENT — reading position 3 does not
/// require positions 1 and 2 to have been read. That is a constraint on the cook, not on this file,
/// and it is what makes "fall back to the highest resident level" a lookup rather than a rebuild.
struct StreamLevel {
    /// Absolute offset within the file named at declaration.
    u64 offset = 0;
    u32 bytes = 0;
};

/// What a caller declares about one asset.
struct StreamDeclaration {
    cy::AssetId id;
    VariantKey variant;
    StreamKind kind = StreamKind::TextureMip;
    /// Where the levels are read from. A loose cooked file or a package mount — the virtual
    /// filesystem has already made that somebody else's problem.
    VirtualPath source;
    /// Coarsest first. At least one: an asset with no base has nothing to fall back to.
    Span<const StreamLevel> levels;
};

struct StreamingStats {
    /// Assets with a declared ladder.
    u64 declared = 0;
    /// `request` calls, and the ones that asked for a level already resident.
    u64 requests = 0;
    u64 requests_satisfied = 0;
    /// Levels admitted, and the bytes they cost.
    u64 levels_loaded = 0;
    u64 bytes_loaded = 0;
    /// Levels dropped to stay inside the budget, and the bytes they returned. The figure the
    /// specification's scenario asks to be recorded.
    u64 evictions = 0;
    u64 bytes_evicted = 0;
    /// Loads that failed. The level stays absent and the asset keeps the level below it.
    u64 load_failures = 0;
    /// Requests for a level above the base that could not be admitted because the budget was
    /// already full of more recently requested levels. Not an error: the frame drew the fallback.
    u64 requests_deferred = 0;
    /// Frames — `update` calls — in which at least one level was requested and not yet resident.
    u64 updates_with_shortfall = 0;

    u64 resident_bytes = 0;
    u64 budget_bytes = 0;
    u64 resident_levels = 0;
};

/// The residency of every declared ladder, and the budget over all of them.
///
/// NOT INTERNALLY THREADED and not thread-safe: `request` is called from the frame that produced
/// the feedback and `update` from the frame loop, both on one thread. The reads it issues run on
/// the async service, and the only cross-thread state is each in-flight load's own slot, which the
/// service writes and `update` reads after the handle completes.
class StreamingSystem {
public:
    StreamingSystem() noexcept;
    ~StreamingSystem();

    StreamingSystem(const StreamingSystem&) = delete;
    StreamingSystem& operator=(const StreamingSystem&) = delete;

    /// Attach to the async service and the mounted namespace. Neither is owned.
    [[nodiscard]] Status start(jobs::AsyncService& async, VirtualFileSystem& files,
                               u64 budget_bytes) noexcept;
    /// Drop every level and forget every declaration. Idempotent.
    void shutdown() noexcept;
    [[nodiscard]] bool is_running() const noexcept { return running_; }

    /// Declare an asset's ladder. Re-declaring one replaces it and drops what was resident, which
    /// is what a hot reload of a re-cooked asset needs.
    ///
    /// Refuses an empty ladder, a level of zero bytes, and a ladder whose declared bytes exceed the
    /// whole budget — the last because an asset that cannot fit even its base would thrash every
    /// update, and failing at declaration names the asset while a caller can still do something.
    [[nodiscard]] Status declare(const StreamDeclaration& declaration) noexcept;

    /// The renderer's feedback, or a distance heuristic's: this asset wants this level.
    ///
    /// Records the want and the moment. It does not read, does not allocate the level and cannot
    /// fail because the budget is full — a request that cannot be met this frame is deferred and
    /// counted, and the caller draws what is resident.
    [[nodiscard]] Status request(cy::AssetId id, VariantKey variant, u32 level,
                                 i64 now_ns) noexcept;

    /// The highest level whose bytes are resident, or 0 for an asset that has only its base.
    /// `kInvalidLevel` when nothing was declared for this asset.
    static constexpr u32 kInvalidLevel = 0xFFFF'FFFFU;
    [[nodiscard]] u32 resident_level(cy::AssetId id, VariantKey variant = {}) const noexcept;

    /// The highest resident level's bytes — the fallback the requirement names. Empty when nothing
    /// is resident yet, which is the state before the first `update` and after a failed read.
    [[nodiscard]] Span<const u8> resident_bytes(cy::AssetId id,
                                                VariantKey variant = {}) const noexcept;

    /// One level's bytes, or empty when that level is not resident. A caller that wants "the best
    /// there is" uses `resident_bytes`; this is for one that wants a specific level or nothing.
    [[nodiscard]] Span<const u8> level_bytes(cy::AssetId id, VariantKey variant,
                                             u32 level) const noexcept;

    /// Collect finished reads, admit what the budget allows, evict what it does not. Called once a
    /// frame. Never blocks: a read still in flight is left in flight.
    void update(i64 now_ns) noexcept;

    /// The budget, in bytes. Set at `start` and re-set whenever the owner of the memory policy
    /// reallocates. Lowering it evicts at the next `update`.
    void set_budget_bytes(u64 bytes) noexcept { budget_bytes_ = bytes; }
    [[nodiscard]] u64 budget_bytes() const noexcept { return budget_bytes_; }

    [[nodiscard]] StreamingStats stats() const noexcept;
    void reset_stats() noexcept;

private:
    /// One position of one ladder.
    struct Level {
        StreamLevel declared;
        Array<u8> bytes;
        /// The value of `requested_at` when this level was last asked for. Eviction order.
        i64 last_request_ns = 0;
        bool resident = false;
        bool in_flight = false;
    };

    /// The shared cell an in-flight read writes and `update` reads once `finished` is set.
    ///
    /// `finished` is released by the service thread and acquired by `collect_finished`, which is
    /// what publishes `bytes` and `ok` across the two threads. A `JobHandle` would have needed the
    /// job system as well as the async service to ask whether it had completed, and this needs one
    /// flag.
    ///
    /// **The cell outlives the read, not the other way round.** `shutdown` waits for
    /// `reads_in_flight_` to reach zero before it frees anything, because a service thread reading
    /// into a freed buffer is exactly the shape of defect M5.5's gate found in the Jolt job bridge
    /// — one run in forty, and invisible in the other thirty-nine.
    struct Read {
        StreamingSystem* owner = nullptr;
        /// The asset, not an index into `entries_`: a re-declaration erases an entry and every
        /// index after it moves, and a read in flight must not land on a different asset's level.
        cy::AssetId id;
        VariantKey variant;
        u32 level = 0;
        Array<u8> bytes;
        VirtualPath source;
        u64 offset = 0;
        u32 size = 0;
        bool ok = false;
        std::atomic<bool> finished{false};

        explicit Read(Allocator& allocator) noexcept : bytes(allocator) {}
    };

    struct Entry {
        cy::AssetId id;
        VariantKey variant;
        StreamKind kind = StreamKind::TextureMip;
        VirtualPath source;
        Array<Level> levels;
        /// The level the most recent feedback asked for. The most recent request WINS rather than
        /// the highest ever seen: a camera that moved away has stopped wanting the finer level,
        /// and a high-water mark would keep loading detail nothing is drawing.
        u32 wanted = 0;
        i64 wanted_at_ns = 0;

        explicit Entry(Allocator& allocator) noexcept : levels(allocator) {}
    };

    [[nodiscard]] Entry* find(cy::AssetId id, VariantKey variant) noexcept;
    [[nodiscard]] const Entry* find(cy::AssetId id, VariantKey variant) const noexcept;

    static void perform_read(void* user) noexcept;

    /// Take completed reads into residency. Returns the bytes admitted.
    void collect_finished() noexcept;
    /// Issue reads for levels wanted and not resident, cheapest-first, while the budget allows.
    void admit(i64 now_ns) noexcept;
    /// Drop the least recently requested non-base levels until the budget is met.
    void evict_to_budget() noexcept;

    [[nodiscard]] u64 in_flight_bytes() const noexcept;

    jobs::AsyncService* async_ = nullptr;
    VirtualFileSystem* files_ = nullptr;
    bool running_ = false;
    u64 budget_bytes_ = 0;
    u64 resident_bytes_total_ = 0;

    Array<UniquePtr<Entry>> entries_{default_allocator()};
    Array<UniquePtr<Read>> reads_{default_allocator()};
    std::atomic<u32> reads_in_flight_{0};
    StreamingStats stats_;
};

}  // namespace cy::assets

#endif  // CY_CORE_ASSETS_STREAMING_H
