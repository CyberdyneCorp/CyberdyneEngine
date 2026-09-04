#pragma once
// Shader hot reload, over M2's file watcher. Task 3.5, and the first real consumer of task 1.4.
//
// `shader-system` — "Hot reload": "In development builds, editing a shader source file SHALL
// trigger recompilation of affected permutations and replacement of the corresponding pipelines,
// without restarting. Compilation SHALL occur on job workers; the previous pipeline SHALL remain in
// use until the new one is ready. A failed compile SHALL keep the previous pipeline and report the
// error."
//
// THE TWO SCENARIOS ARE TWO PROPERTIES OF THIS CLASS, and neither needs a GPU to test:
//
//   "Live shader iteration"            an edit becomes a replaced library entry, so a consumer
//                                      holding a `ShaderVariantId` sees new code with no restart.
//   "Broken shader does not break      a failed rebuild calls `ShaderLibrary::replace()` zero
//    the frame"                        times. The old artefact is still there, its generation has
//                                      not moved, and the pipeline built from it is still valid.
//
// WHY THE SPLIT BETWEEN `poll` AND `rebuild`. `cy::assets::FileWatcher` reads files, so it must be
// called from a thread where blocking is legal and never from a job body — its own header says so.
// Compilation, by contrast, is exactly what `shader-system` wants on job workers. So `poll()`
// detects (on the caller's thread, at a human rate) and `rebuild()` compiles (anywhere the caller
// likes, one caller at a time). The queue between them is this object's own, and the split is the
// reason a shader edit does not stall the frame that noticed it.
//
// THE WATCHER'S SETTLE PERIOD IS DOING REAL WORK HERE. An editor writing a file in place is briefly
// holding a truncated file, and compiling that produces a spurious error and a spurious "your
// shader is broken" in the viewport. `FileWatcher` reports a change only once its fingerprint has
// held for `settle_ns`; this class inherits that for free and does not re-implement it.

#include <cy/backends/shader/cache.h>
#include <cy/backends/shader/compiler.h>
#include <cy/backends/shader/library.h>
#include <cy/backends/shader/pipeline.h>
#include <cy/backends/shader/source.h>
#include <cy/core/assets/watch.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>

namespace cy::shader {

struct HotReloadConfig {
    assets::FileWatcherConfig watcher{};
    /// The most modules that may be waiting to be rebuilt. A save-all across a large shader tree
    /// is the case this bounds; past it, the oldest pending entries stand and the overflow is
    /// counted, because dropping the *new* edit would be the wrong half to lose.
    u32 max_pending = 256;
};

struct ShaderReloadEvent {
    Name module_name;
    assets::FileChange change = assets::FileChange::Modified;
    /// False when the source could not be re-read — a file deleted between the watcher reporting it
    /// and the registry reading it. The module then stays as it was.
    bool source_reloaded = false;
};

using ShaderReloadObserver = void (*)(void* user, const ShaderReloadEvent& event) noexcept;

/// How `rebuild` finds the axes a module varies over, since the library records a key and not the
/// set that produced it. Null means "no permutations", which is correct for a shader that declares
/// no axes and is the common case for an engine utility shader.
struct PermutationLookup {
    const PermutationSet* (*lookup)(void* user, Name module_name) noexcept = nullptr;
    void* user = nullptr;

    [[nodiscard]] const PermutationSet* operator()(Name module_name) const noexcept {
        return lookup != nullptr ? lookup(user, module_name) : nullptr;
    }
};

struct RebuildOptions {
    OptimizationLevel optimization = OptimizationLevel::Performance;
    DebugInfoLevel debug_info = DebugInfoLevel::Full;
    u32 spirv_version = kSpirv1_5;
    PermutationLookup permutations;
    /// Optional. When set, every state naming a replaced program is invalidated, which is the
    /// "replacement of the corresponding pipelines" half of the requirement.
    PipelineStateCache* pipelines = nullptr;
};

struct HotReloadStats {
    u64 polls = 0;
    u64 events = 0;
    u64 modules_queued = 0;
    u64 modules_rebuilt = 0;
    u64 variants_replaced = 0;
    u64 rebuild_failures = 0;
    u64 pipelines_invalidated = 0;
    /// Edits dropped because `max_pending` was reached.
    u64 overflowed = 0;
};

struct RebuildResult {
    u32 modules_rebuilt = 0;
    u32 variants_replaced = 0;
    u32 failures = 0;
    u32 pipelines_invalidated = 0;
};

/// Watches a shader source tree and rebuilds what changed.
class ShaderHotReload {
public:
    explicit ShaderHotReload(Allocator& allocator) noexcept;

    ShaderHotReload(const ShaderHotReload&) = delete;
    ShaderHotReload& operator=(const ShaderHotReload&) = delete;

    /// Attach to a registry and start watching its root. The registry and the filesystem must
    /// outlive this object.
    [[nodiscard]] Status start(SourceRegistry& sources, assets::VirtualFileSystem& files,
                               const HotReloadConfig& config) noexcept;
    void stop() noexcept;
    [[nodiscard]] bool is_running() const noexcept { return sources_ != nullptr; }

    /// Take the baseline without reporting. What a host calls once the tree is loaded, so the first
    /// poll does not report every existing file as new.
    [[nodiscard]] Status prime(i64 now_ns) noexcept;

    /// Examine the tree and queue the modules that changed. Returns how many were queued. Call from
    /// a thread where blocking is legal, a few times a second — not from a job body and not every
    /// frame; the watcher's own header explains the cost.
    [[nodiscard]] Expected<u32, Error> poll(i64 now_ns) noexcept;

    void set_observer(ShaderReloadObserver observer, void* user) noexcept;

    [[nodiscard]] Span<const Name> pending() const noexcept;
    [[nodiscard]] bool is_pending(Name module_name) const noexcept;
    /// Queue a module by hand. What a generator calls after re-emitting generated source: a
    /// generated module has no file for the watcher to see, and this is how it joins the same
    /// rebuild path rather than getting one of its own.
    [[nodiscard]] Status invalidate(Name module_name) noexcept;

    /// Recompile every queued module's variants and swap the ones that succeeded into `library`.
    ///
    /// A module whose compilation fails leaves the library untouched and its diagnostics in
    /// `diagnostics`; it leaves the queue either way, so a broken shader is reported once per edit
    /// rather than once per frame. Safe on a job worker, one caller at a time.
    [[nodiscard]] Expected<RebuildResult, Error> rebuild(ShaderCompiler& compiler,
                                                         ShaderLibrary& library,
                                                         const RebuildOptions& options,
                                                         DiagnosticLog& diagnostics) noexcept;

    [[nodiscard]] const HotReloadStats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = HotReloadStats{}; }

private:
    static void on_watch_event(void* user, const assets::WatchEvent& event) noexcept;
    void handle_event(const assets::WatchEvent& event) noexcept;
    [[nodiscard]] Status queue(Name module_name) noexcept;
    /// Rebuild one module's variants. Returns how many were replaced, or zero on failure.
    [[nodiscard]] Expected<u32, Error> rebuild_module(Name module_name, ShaderCompiler& compiler,
                                                      ShaderLibrary& library,
                                                      const RebuildOptions& options,
                                                      DiagnosticLog& diagnostics) noexcept;

    Allocator* allocator_;
    SourceRegistry* sources_ = nullptr;
    assets::FileWatcher watcher_;
    HotReloadConfig config_{};
    Array<Name> pending_;
    ShaderReloadObserver observer_ = nullptr;
    void* observer_user_ = nullptr;
    Status sweep_status_ = ok();
    HotReloadStats stats_{};
};

}  // namespace cy::shader
