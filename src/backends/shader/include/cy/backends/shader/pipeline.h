#pragma once
// Pipeline state object management: the key, the manifest, the warm-up and the fallback. Task 3.7.
//
// `shader-system` — "Pipeline state object management": "Pipeline state compilation SHALL be
// managed centrally and identically for every pipeline, since a first-use compilation stall is a
// property of the API, not of any one renderer pipeline." The engine SHALL collect the states a
// project actually uses, cook them into a manifest, warm the cache from it at load time with
// progress reporting, use a generic fallback for any state not yet compiled, and compile the
// missing state asynchronously. "Blocking the frame to compile a pipeline state SHALL NOT occur in
// shipping builds."
//
// WHAT LIVES HERE AND WHAT DOES NOT. This module owns the KEY, the MANIFEST and the STATE MACHINE.
// It does not own a device: creating a `rhi::GraphicsPipelineHandle` is the renderer's, and a
// pipeline manager that also owned device lifetime would be untestable without one — which is the
// same mistake as writing the null RHI backend last. So `PipelineStateCache` is given a
// `PipelineBuilder` — a function pointer and a user pointer — and calls it when a state has to
// exist. A test supplies one that counts calls; the renderer supplies one that calls
// `rhi::Device::create_graphics_pipeline`.
//
// THE KEY IS DERIVED FROM STABLE INPUTS ONLY, which is the same requirement
// `rendering-architecture` puts on draw sort keys (design.md §6) and it matters here for the same
// reason: a manifest cooked on a build machine has to name the same states the game asks for at run
// time. So the key hashes content hashes and enumerator values — never a handle, a pointer, or the
// order anything was created in. `shader-system` fixes the contents: "the shader stages, render
// target formats, depth and stencil state, blend state, rasteriser state, sample count, and the
// permutation key".
//
// THE DISK PIPELINE CACHE IS A SEPARATE KEY AND A SEPARATE LIFETIME. `shader-system`: "The runtime
// SHALL maintain a pipeline cache on disk keyed by device, driver version, and engine version ...
// when the driver version changes, the cache key SHALL differ, the old cache SHALL be discarded,
// and pipelines rebuilt." That is `PipelineDiskCacheKey` below; it names a *file*, and the driver
// upgrade that invalidates it is a different file name rather than a validity check inside the old
// one, because a driver's own blob format is opaque and asking it whether it is stale is exactly
// what it cannot answer.

#include <cy/backends/rhi/pipeline.h>
#include <cy/backends/shader/library.h>
#include <cy/core/assets/hash.h>
#include <cy/core/assets/path.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>

#include <string_view>

namespace cy::shader {

inline constexpr u32 kPipelineManifestMagic = 0x4D50'5943U;  // 'CYPM', little-endian
inline constexpr u32 kPipelineManifestVersion = 1;

/// Which of the two pipeline kinds a state is. The RHI types them separately —
/// `rhi::GraphicsPipelineHandle` and `rhi::ComputePipelineHandle` are distinct handle types, so a
/// compute pipeline cannot be bound where a graphics one belongs — and this module keeps that
/// distinction rather than erasing it to a common integer. `shader-system` requires compute shaders
/// to be "first-class artefacts with the same authoring, reflection, caching and hot-reload path";
/// same path is not the same handle.
enum class PipelineKind : u8 { Graphics = 0, Compute = 1 };

const char* pipeline_kind_name(PipelineKind kind) noexcept;

/// A built pipeline, of whichever kind. Exactly one of the two handles is valid.
struct PipelineObject {
    PipelineKind kind = PipelineKind::Graphics;
    rhi::GraphicsPipelineHandle graphics;
    rhi::ComputePipelineHandle compute;

    [[nodiscard]] bool is_valid() const noexcept {
        return kind == PipelineKind::Graphics ? !graphics.is_null() : !compute.is_null();
    }
};

/// Everything the specification requires a pipeline state key to include.
struct PipelineStateInputs {
    /// The content hash of each participating program, in stage order. A graphics state names its
    /// vertex and fragment modules; a compute state names one.
    Span<const assets::ContentHash> programs;
    Span<const rhi::ColorAttachmentState> color_attachments;
    rhi::DepthStencilState depth_stencil{};
    rhi::RasterisationState rasterisation{};
    Span<const rhi::VertexBinding> vertex_bindings;
    Span<const rhi::VertexAttribute> vertex_attributes;
    rhi::PrimitiveTopology topology = rhi::PrimitiveTopology::TriangleList;
    u32 sample_count = 1;
    u32 view_mask = 0;
    PermutationKey permutation;
    PipelineKind kind = PipelineKind::Graphics;
};

struct PipelineStateKey {
    assets::ContentHash hash;

    friend bool operator==(const PipelineStateKey& a, const PipelineStateKey& b) noexcept {
        return a.hash == b.hash;
    }
    friend bool operator!=(const PipelineStateKey& a, const PipelineStateKey& b) noexcept {
        return !(a == b);
    }
};

[[nodiscard]] PipelineStateKey derive_pipeline_state_key(
    const PipelineStateInputs& inputs) noexcept;

/// The file the driver's own pipeline blob is kept in. Not the state key: this names the *cache*,
/// and every state in the process shares it.
struct PipelineDiskCacheKey {
    /// The device's reported name, its driver version, and the engine's version. All three change
    /// the file name; none of them is interpreted.
    std::string_view device_name;
    u32 vendor_id = 0;
    u32 device_id = 0;
    u32 driver_version = 0;
    std::string_view engine_version;
};

/// `<root>/<16 hex digits>.pcache`. Deterministic, so the same machine finds yesterday's file, and
/// different for a different driver, so it does not.
[[nodiscard]] Expected<assets::VirtualPath, Error> pipeline_cache_path(
    const assets::VirtualPath& root, const PipelineDiskCacheKey& key) noexcept;

/// Where a pipeline state is in its life.
enum class PipelineStatus : u8 {
    /// Not known to the cache. The first draw that asks for it puts it in `Compiling` and gets the
    /// fallback.
    Missing = 0,
    /// A build has been requested and has not finished. The fallback is used meanwhile.
    Compiling = 1,
    /// Built and usable.
    Ready = 2,
    /// The build failed. The fallback is used and the failure is reported once, not once per draw.
    Failed = 3,
};

const char* pipeline_status_name(PipelineStatus status) noexcept;

/// One recorded state: the key, and enough of the inputs to rebuild it.
///
/// The manifest stores the key and the program hashes rather than the whole `PipelineStateInputs`,
/// because the rest of the state is reconstructed by the renderer from the same material and pass
/// that asked for it the first time. What the manifest is *for* is knowing which states to warm,
/// and a key plus its programs is enough to ask the renderer to produce it.
struct PipelineManifestEntry {
    PipelineStateKey key;
    assets::ContentHash programs[2];
    u8 program_count = 0;
    /// How many times the state was asked for during collection. Warming goes in descending order,
    /// so the states a level actually leans on are ready first when warming is interrupted.
    u32 uses = 0;
};

/// The states a project used, collected during play and shipped with the game.
class PipelineManifest {
public:
    explicit PipelineManifest(Allocator& allocator) noexcept;

    PipelineManifest(const PipelineManifest&) = delete;
    PipelineManifest& operator=(const PipelineManifest&) = delete;
    PipelineManifest(PipelineManifest&&) noexcept = default;
    PipelineManifest& operator=(PipelineManifest&&) noexcept = default;

    /// Record one use. Idempotent by key: a state seen a thousand times is one entry with
    /// `uses == 1000`.
    [[nodiscard]] Status record(const PipelineStateKey& key,
                                Span<const assets::ContentHash> programs) noexcept;

    [[nodiscard]] usize size() const noexcept { return entries_.size(); }
    [[nodiscard]] const PipelineManifestEntry* entry_at(usize index) const noexcept;
    [[nodiscard]] bool contains(const PipelineStateKey& key) const noexcept;

    /// Order by descending use count, then by key, so a manifest cooked twice from the same play
    /// session is byte-identical. Called by `serialize`.
    void sort() noexcept;

    [[nodiscard]] Status serialize(Array<u8>& out) const noexcept;
    [[nodiscard]] static Expected<PipelineManifest, Error> parse(Allocator& allocator,
                                                                 Span<const u8> bytes) noexcept;

    void clear() noexcept;

private:
    [[nodiscard]] usize find(const PipelineStateKey& key) const noexcept;

    Array<PipelineManifestEntry> entries_;
};

/// How the cache asks for a pipeline to exist. Returns the built handle, or an error, and never
/// blocks in a shipping build — the caller runs it on a job worker.
using PipelineBuilder = Expected<PipelineObject, Error> (*)(
    void* user, const PipelineStateKey& key, Span<const assets::ContentHash> programs) noexcept;

/// Reported by `warm`, once per entry, so loading can show progress.
using PipelineWarmObserver = void (*)(void* user, usize completed, usize total,
                                      const PipelineStateKey& key, bool built) noexcept;

struct PipelineCacheStats {
    u64 requests = 0;
    u64 hits = 0;
    /// Requests answered with the fallback because the state was not ready.
    u64 fallbacks = 0;
    u64 builds = 0;
    u64 build_failures = 0;
    u64 warmed = 0;
};

/// The central pipeline state table: one place, every pipeline.
class PipelineStateCache {
public:
    explicit PipelineStateCache(Allocator& allocator) noexcept;

    PipelineStateCache(const PipelineStateCache&) = delete;
    PipelineStateCache& operator=(const PipelineStateCache&) = delete;

    /// The builder every miss goes through, and the fallback handed out meanwhile.
    ///
    /// `shader-system` requires a fallback "so a missing state causes a temporary visual
    /// approximation rather than a hitch". A cache with no fallback set still works and reports
    /// `Missing`; it just cannot answer a draw, which is the honest behaviour before the renderer
    /// has created one.
    void set_builder(PipelineBuilder builder, void* user) noexcept;
    void set_fallback(const PipelineObject& fallback) noexcept { fallback_ = fallback; }
    [[nodiscard]] const PipelineObject& fallback() const noexcept { return fallback_; }

    /// The pipeline for a state, or the fallback.
    ///
    /// A miss records the state as `Compiling` and returns the fallback; it does not build inline,
    /// because that is the frame stall this whole requirement exists to remove. `build_pending()`
    /// is what a job worker calls to do the work.
    struct Request {
        PipelineObject pipeline;
        PipelineStatus status = PipelineStatus::Missing;
        /// True when `pipeline` is the fallback rather than the state that was asked for.
        bool is_fallback = false;
    };
    [[nodiscard]] Request request(const PipelineStateKey& key,
                                  Span<const assets::ContentHash> programs) noexcept;

    /// Build one state that is waiting. Returns false when none was. Safe to call from a job
    /// worker as long as one worker at a time calls it — the same contract the rest of this module
    /// states, and the reason it is a separate call rather than a thread this class started.
    [[nodiscard]] Expected<bool, Error> build_pending() noexcept;

    /// Build every state in a manifest, reporting progress. This is load-time warming: "required
    /// pipeline states SHALL be compiled during loading, not on first draw".
    [[nodiscard]] Status warm(const PipelineManifest& manifest, PipelineWarmObserver observer,
                              void* user) noexcept;

    [[nodiscard]] PipelineStatus status_of(const PipelineStateKey& key) const noexcept;
    [[nodiscard]] usize size() const noexcept { return states_.size(); }
    [[nodiscard]] usize pending_count() const noexcept;

    /// Drop a state so it is rebuilt on next use. Hot reload calls this for every state whose
    /// program was replaced.
    [[nodiscard]] Status invalidate(const PipelineStateKey& key) noexcept;
    /// Drop every state naming `program`. The hot-reload path's real entry point: a rebuilt shader
    /// invalidates the pipelines that used it and nothing else.
    [[nodiscard]] u32 invalidate_by_program(const assets::ContentHash& program) noexcept;

    [[nodiscard]] const PipelineCacheStats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = PipelineCacheStats{}; }
    void clear() noexcept;

private:
    struct State {
        PipelineStateKey key;
        assets::ContentHash programs[2];
        u8 program_count = 0;
        PipelineStatus status = PipelineStatus::Missing;
        PipelineObject pipeline;
    };

    [[nodiscard]] usize find(const PipelineStateKey& key) const noexcept;
    [[nodiscard]] Expected<usize, Error> intern(const PipelineStateKey& key,
                                                Span<const assets::ContentHash> programs) noexcept;
    [[nodiscard]] bool build_at(usize index) noexcept;

    Array<State> states_;
    PipelineBuilder builder_ = nullptr;
    void* builder_user_ = nullptr;
    PipelineObject fallback_;
    PipelineCacheStats stats_;
};

}  // namespace cy::shader
