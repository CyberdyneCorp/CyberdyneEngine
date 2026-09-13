#ifndef CY_SHADER_PIPELINE_H
#define CY_SHADER_PIPELINE_H
// Pipeline state object management: the key, the manifest, warming, and the fallback. Task 3.7.
//
// `shader-system` — "Pipeline state object management": pipeline state compilation is managed
// **centrally and identically for every pipeline**, "since a first-use compilation stall is a
// property of the API, not of any one renderer pipeline". The engine collects the states a project
// uses, cooks them into a manifest, warms the cache from it at load with progress reporting, uses a
// generic fallback for any state not yet compiled, and compiles the missing state asynchronously.
// **Blocking the frame to compile a pipeline state does not occur in shipping builds.**
//
// --- WHY THIS LIVES IN THE SHADER MODULE AND NOT IN THE RENDERER
// ----------------------------------
//
// Because the specification's third scenario — "Every pipeline benefits" — is the whole point: a
// project using the visibility-buffer pipeline or a custom pipeline gets the same manifest, warming
// and fallback as Forward+. A manager that lived in the forward renderer would be one the second
// renderer reimplements, and the reimplementation is where the fallback stops working.
//
// --- WHAT THIS MODULE OWNS AND WHAT IT DOES NOT
// ----------------------------------------------------
//
// It owns the **key**, the **manifest**, the **policy** and the **statistics**. It creates no
// device object: `PipelineBuilder` is the interface the RHI implements, and everything here is
// expressed over opaque `PipelineHandle`s the builder mints. That is what keeps Vulkan out of this
// module entirely — and it is also what makes the whole of the warming and fallback behaviour
// testable with a fake builder and no GPU, which is exactly how the tests beside this header work.
//
// A pipeline state key includes, per the specification: the shader stages, render target formats,
// depth and stencil state, blend state, rasteriser state, sample count, and the permutation key.
// `PipelineStateKey` below has a field for each, and `hash()` folds all of them — a key that
// omitted one would serve a pipeline built for a different render target format, which is undefined
// behaviour that usually looks like a corrupted frame rather than a crash.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/shader/permutation.h>
#include <cy/shader/shader.h>

namespace cy::shader {

/// The render-target formats a pipeline is built against, as an engine-side enumeration.
///
/// Not a Vulkan format: the RHI maps it. The set is what M3's frame actually uses plus the depth
/// format reversed-Z needs, and it grows with the render targets rather than ahead of them.
enum class RenderTargetFormat : u8 {
    None = 0,
    RgbaUnorm8 = 1,
    RgbaSrgb8 = 2,
    RgbaFloat16 = 3,
    RgFloat16 = 4,
    R11G11B10Float = 5,
    Depth32Float = 6,
    Depth24Stencil8 = 7,
};

const char* render_target_format_name(RenderTargetFormat format) noexcept;

/// The largest number of colour attachments a pipeline state may name.
inline constexpr u32 kMaxColorAttachments = 8;
/// The largest number of stages one pipeline may bind: vertex, fragment, geometry, two tessellation
/// stages, or task and mesh — the graphics maximum. Compute and ray pipelines use fewer.
inline constexpr u32 kMaxPipelineStages = 5;

/// The fixed-function state a pipeline bakes in. Deliberately a flat POD: the key is hashed and
/// compared by bytes, and a struct with a pointer in it could not be.
struct PipelineFixedState {
    /// Reversed-Z, per design.md §3: cleared to 0, compared GreaterEqual. `true` is the engine's
    /// convention and `false` exists so a tool that needs the other comparison can say so rather
    /// than reaching around this struct.
    bool depth_test = true;
    bool depth_write = true;
    bool depth_greater_equal = true;
    bool stencil_test = false;
    bool blend_enable = false;
    bool cull_back = true;
    bool front_face_counter_clockwise = true;
    bool wireframe = false;
    u8 sample_count = 1;
    u8 stencil_read_mask = 0xFF;
    u8 stencil_write_mask = 0xFF;
    u8 reserved = 0;
};

/// Everything that identifies one pipeline state object.
struct PipelineStateKey {
    /// The content hash of each stage's code blob, in stage order. A stage that is absent is a zero
    /// hash. Hashes rather than names: two entry points with the same name in two libraries are two
    /// pipelines, and the code is what the driver actually compiles.
    ContentHash stages[kMaxPipelineStages];
    /// Which stage each slot holds, so the key distinguishes a vertex/fragment pair from a
    /// task/mesh pair whose blobs happened to land in the same slots.
    Stage stage_kinds[kMaxPipelineStages] = {};
    u8 stage_count = 0;
    RenderTargetFormat color_formats[kMaxColorAttachments] = {};
    u8 color_count = 0;
    RenderTargetFormat depth_format = RenderTargetFormat::None;
    PipelineFixedState fixed;
    PermutationKey permutation;
    /// The layout the stages derived — see `reflection.h`. Two states sharing a layout share the
    /// layout object; a state whose layout differs is a different state even when everything else
    /// matches, because the descriptor bindings are part of what the driver compiles.
    ContentHash layout;

    /// Fold every field, in a fixed order. The pipeline cache's key on disk.
    [[nodiscard]] ContentHash hash() const noexcept;
};

/// An opaque handle onto a device pipeline. Minted by the builder; meaningless to this module
/// beyond equality and validity, which is what keeps the RHI's types out of this header.
struct PipelineHandle {
    u64 value = 0;

    [[nodiscard]] bool valid() const noexcept { return value != 0; }
    friend bool operator==(PipelineHandle a, PipelineHandle b) noexcept {
        return a.value == b.value;
    }
};

/// What a request for a pipeline got.
enum class PipelineAvailability : u8 {
    /// The requested state, ready to bind.
    Ready = 0,
    /// The generic fallback, while the real one compiles. The draw happens with a temporary visual
    /// approximation instead of a hitch, which is the requirement.
    Fallback = 1,
    /// Neither: no fallback is registered and the state is not compiled. A configuration error, and
    /// the only outcome that must be loud.
    Unavailable = 2,
};

struct PipelineRequest {
    PipelineHandle handle;
    PipelineAvailability availability = PipelineAvailability::Unavailable;
};

/// The device side, implemented by the RHI.
class PipelineBuilder {
public:
    virtual ~PipelineBuilder() = default;

    PipelineBuilder(const PipelineBuilder&) = delete;
    PipelineBuilder& operator=(const PipelineBuilder&) = delete;
    PipelineBuilder(PipelineBuilder&&) = delete;
    PipelineBuilder& operator=(PipelineBuilder&&) = delete;

    /// Create the pipeline. Called on a job worker for an asynchronous compile and on the loading
    /// thread during warming; never on the render thread in a shipping build.
    [[nodiscard]] virtual Expected<PipelineHandle, Error> build(
        const PipelineStateKey& key) noexcept = 0;

    /// Destroy one. Called when the registry is torn down, and when hot reload replaces a pipeline
    /// whose predecessor has left every frame in flight.
    virtual void destroy(PipelineHandle handle) noexcept = 0;

    /// The device's own pipeline cache blob, persisted to disk between runs.
    ///
    /// `shader-system` — "Cache reused across runs" and "Driver update invalidates the cache": the
    /// key is the device, the driver version and the engine version, which the RHI knows and this
    /// module does not. `cache_key()` is where it reports them.
    [[nodiscard]] virtual ContentHash cache_key() const noexcept = 0;
    [[nodiscard]] virtual Status serialise_cache(Array<u8>& out) noexcept = 0;
    [[nodiscard]] virtual Status load_cache(Span<const u8> bytes) noexcept = 0;

protected:
    PipelineBuilder() = default;
};

/// The cooked list of states a project uses. `shader-system`'s "pipeline manifest".
///
/// Collected during play, development and automated traversal, shipped with the game, and used to
/// warm the cache at load time. The format is a header and an array of keys, deterministic in entry
/// order so that two collection runs over the same play session produce comparable manifests.
class PipelineManifest {
public:
    explicit PipelineManifest(Allocator& allocator) noexcept;

    PipelineManifest(const PipelineManifest&) = delete;
    PipelineManifest& operator=(const PipelineManifest&) = delete;

    /// Record a state. Deduplicates by hash, so a manifest collected over an hour of play is the
    /// set of states used and not the sequence of draws.
    [[nodiscard]] Status record(const PipelineStateKey& key) noexcept;

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(keys_.size()); }
    [[nodiscard]] const PipelineStateKey& at(u32 index) const noexcept { return keys_[index]; }

    [[nodiscard]] Status serialise(Array<u8>& out) const noexcept;
    [[nodiscard]] static Expected<PipelineManifest, Error> parse(Span<const u8> bytes,
                                                                 Allocator& allocator) noexcept;

private:
    Array<PipelineStateKey> keys_;
    Array<ContentHash> hashes_;
};

struct PipelineStats {
    u64 requests = 0;
    u64 ready = 0;
    u64 fallbacks = 0;
    u64 built = 0;
    u64 build_failures = 0;
    /// States warmed from the manifest before the first frame.
    u64 warmed = 0;
    u64 total_build_ns = 0;
    /// Pipelines built on the render thread while a frame was waiting. **This number must be zero
    /// in a shipping build**, and it is the one the specification's requirement is measured by.
    u64 blocking_builds = 0;
};

/// The central pipeline manager: one per device.
class PipelineRegistry {
public:
    PipelineRegistry(PipelineBuilder& builder, Allocator& allocator) noexcept;

    PipelineRegistry(const PipelineRegistry&) = delete;
    PipelineRegistry& operator=(const PipelineRegistry&) = delete;
    ~PipelineRegistry();

    /// The pipeline used for a state that is not compiled yet. Registered once, at start-up, from a
    /// state the manifest always contains.
    [[nodiscard]] Status set_fallback(PipelineHandle handle) noexcept;

    /// Build every state in the manifest, reporting progress.
    ///
    /// `progress` is called with (completed, total) after each state, which is what a loading
    /// screen needs; it may be null. Warming continues past a failure and reports the count,
    /// because one state a driver refuses should not stop a game from loading.
    using ProgressFn = void (*)(void* context, u32 completed, u32 total);
    [[nodiscard]] Status warm(const PipelineManifest& manifest, ProgressFn progress,
                              void* context) noexcept;

    /// Ask for a state.
    ///
    /// Returns `Ready` when it is compiled, `Fallback` when it is not and a fallback exists — and
    /// **queues the state for asynchronous compilation** in that case. `allow_blocking` is the
    /// escape hatch a development build and a cook step use; it is false by default so that the
    /// shipping requirement holds by construction rather than by remembering to pass false.
    [[nodiscard]] PipelineRequest acquire(const PipelineStateKey& key,
                                          bool allow_blocking = false) noexcept;

    /// Build one queued state. Called from a job worker; returns false when the queue is empty.
    ///
    /// One at a time rather than draining, so the caller decides how much of a frame's slack goes
    /// into pipeline compilation.
    [[nodiscard]] bool build_one_pending() noexcept;

    [[nodiscard]] u32 pending() const noexcept { return static_cast<u32>(pending_.size()); }
    [[nodiscard]] u32 compiled() const noexcept { return static_cast<u32>(entries_.size()); }
    [[nodiscard]] PipelineStats stats() const noexcept { return stats_; }

    /// Every state built so far, as a manifest ready to cook. This is how the collection half of
    /// `shader-system`'s requirement is served: play the game, write the manifest.
    [[nodiscard]] Status collect(PipelineManifest& out) const noexcept;

private:
    struct Entry {
        ContentHash hash;
        PipelineHandle handle;
    };

    [[nodiscard]] const Entry* find(const ContentHash& hash) const noexcept;
    [[nodiscard]] Status build_and_record(const PipelineStateKey& key) noexcept;

    PipelineBuilder* builder_;
    Array<Entry> entries_;
    Array<PipelineStateKey> keys_;
    Array<PipelineStateKey> pending_;
    PipelineHandle fallback_;
    PipelineStats stats_;
};

}  // namespace cy::shader

#endif  // CY_SHADER_PIPELINE_H
