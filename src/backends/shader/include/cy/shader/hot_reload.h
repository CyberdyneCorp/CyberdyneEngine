#ifndef CY_SHADER_HOT_RELOAD_H
#define CY_SHADER_HOT_RELOAD_H
// Shader hot reload, over the file watcher task 1.4 landed in `core-assets-and-io`. Task 3.5.
//
// `shader-system` — "Hot reload": in development builds, editing a shader source file triggers
// recompilation of the **affected permutations** and replacement of the corresponding pipelines,
// without restarting. Compilation occurs on job workers; **the previous pipeline stays in use until
// the new one is ready**; a failed compile keeps the previous pipeline and reports the error.
//
// --- THE TWO PROPERTIES THAT MAKE THIS MORE THAN A RECOMPILE
// ---------------------------------------
//
// **"Affected" is a transitive-import question, not a filename question.** Editing `cy/brdf.slang`
// affects every shader that imports it, directly or through another module. This class keeps the
// import graph the compiler reported — `CompileRecord::imports` — and walks it backwards from the
// changed file. Without that, an edit to the standard library reloads nothing and the developer
// concludes hot reload is broken; with a "reload everything" fallback instead, an edit to the
// standard library recompiles a project's whole shader set and the developer concludes the same.
//
// **A broken shader must not break the frame.** The new pipeline is built beside the old one and
// swapped only on success. `ReloadOutcome::Failed` carries the diagnostics and leaves the previous
// generation bound, which is the specification's "Broken shader does not break the frame" scenario.
// The failure is *sticky* — the entry keeps failing state until an edit compiles — so the error
// stays on screen rather than flickering away on the next poll that found nothing.
//
// --- WHY THE SETTLING RULE MATTERS HERE
// ------------------------------------------------------------
//
// `cy::assets::FileWatcher` reports a file only once two polls agree about its size and
// modification time, precisely so a loader is never handed a file an editor is still writing. A
// shader compiler handed half a file produces a syntax error at the truncation point, which is a
// confusing thing to show a developer who has just pressed save. The watcher's one-poll latency is
// the price, and this class pays it rather than reimplementing the check.

#include <cy/core/assets/hot_reload.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/shader/compiler.h>
#include <cy/shader/source.h>

namespace cy::shader {

/// What a reload attempt did to one entry.
enum class ReloadOutcome : u8 {
    /// Recompiled and swapped in.
    Replaced = 0,
    /// Recompiled to byte-identical code. Nothing was swapped, because swapping a pipeline that has
    /// not changed costs a stall for no reason — and a formatting-only edit is the common case.
    Unchanged = 1,
    /// The compile failed. The previous generation stays bound and `diagnostics` says why.
    Failed = 2,
};

const char* reload_outcome_name(ReloadOutcome outcome) noexcept;

/// One tracked shader: the request that produces it, and what the last compile knew.
struct ReloadEntry {
    CompileRequest request;
    /// The code hash of the generation currently bound. Compared against a fresh compile to decide
    /// `Replaced` versus `Unchanged`.
    ContentHash current_code_hash;
    /// Incremented on every `Replaced`. What a renderer compares to notice it must rebuild a
    /// pipeline, without being told.
    u32 generation = 0;
    /// True while the last attempt failed. Sticky until a compile succeeds.
    bool failing = false;
};

/// What one `poll()` did, for the log line and for the test.
struct ReloadReport {
    u32 files_changed = 0;
    u32 entries_considered = 0;
    u32 replaced = 0;
    u32 unchanged = 0;
    u32 failed = 0;
    u64 compile_ns = 0;
};

/// Watches shader sources and recompiles what an edit affects.
///
/// Development builds only — `shader-system` is explicit that runtime shader compilation exists
/// only there. It is a compile error to *use* this in a shipping build only in the sense that the
/// Slang compiler is absent and `current_compiler()` reports unavailable; the class itself is
/// harmless and is compiled everywhere so that its tests run in every profile.
class ShaderHotReload {
public:
    ShaderHotReload(SourceStore& sources, Allocator& allocator) noexcept;

    ShaderHotReload(const ShaderHotReload&) = delete;
    ShaderHotReload& operator=(const ShaderHotReload&) = delete;

    /// Watch a directory of shader sources. Recursive, because the standard library is a tree.
    [[nodiscard]] Status watch_directory(const char* path) noexcept;
    /// Watch one file — used for a shader outside any watched root.
    [[nodiscard]] Status watch_file(const char* path) noexcept;

    /// Track a compiled shader so an edit to its source, or to anything it imports, reloads it.
    ///
    /// `imports` is the module list the compiler resolved, which is why it is a parameter rather
    /// than something this class works out: only the compiler knows which imports were actually
    /// followed after generics and conditional compilation.
    [[nodiscard]] Expected<u32, Error> track(const CompileRequest& request,
                                             const ContentHash& code_hash,
                                             Span<const SourceId> imports) noexcept;

    /// Update the import set of a tracked entry after a recompile. An edit can add an import.
    [[nodiscard]] Status set_imports(u32 entry, Span<const SourceId> imports) noexcept;

    /// Poll the watcher and recompile what changed. Returns what happened.
    ///
    /// Compilation happens inline here rather than on a job worker. That is a deliberate M3
    /// simplification and it is called out rather than hidden: `shader-system` requires job-worker
    /// compilation, the seam is `compile_one()` below, and moving it onto `cy::jobs` is a change to
    /// this one function. Inline is honest at M3 because the only caller is the sample's frame loop
    /// and a stall there is visible and small; it is not honest once the editor viewport exists at
    /// M5, which is the milestone that must finish this.
    [[nodiscard]] Expected<ReloadReport, Error> poll() noexcept;

    /// Recompile one tracked entry regardless of whether anything changed. The editor's "recompile
    /// this shader" command, and the unit tests' way of exercising the swap without a filesystem.
    [[nodiscard]] Expected<ReloadOutcome, Error> compile_one(u32 entry) noexcept;

    [[nodiscard]] u32 entry_count() const noexcept { return static_cast<u32>(entries_.size()); }
    [[nodiscard]] const ReloadEntry& entry(u32 index) const noexcept { return entries_[index]; }
    /// The code of the most recent successful compile of an entry. Empty until one has happened.
    [[nodiscard]] Span<const u32> code(u32 index) const noexcept;
    /// The diagnostics of the most recent attempt. Kept across polls so a failure stays on screen.
    [[nodiscard]] Span<const Diagnostic> diagnostics(u32 index) const noexcept;

    [[nodiscard]] const assets::FileWatcher& watcher() const noexcept { return watcher_; }

private:
    /// A tracked entry, plus the storage the public accessors hand out.
    struct Tracked {
        ReloadEntry entry;
        Array<SourceId> imports;
        Array<u32> code;
        Array<Diagnostic> diagnostics;
    };

    /// True when `entry` reads `source`, directly or as the module it was compiled from.
    [[nodiscard]] bool affected_by(const Tracked& tracked, SourceId source) const noexcept;

    SourceStore* sources_;
    Allocator* allocator_;
    assets::FileWatcher watcher_;
    Array<Tracked> entries_;
    Array<assets::FileEvent> events_;
    /// Sources whose text changed in this poll. A member rather than a local so that a poll does
    /// not allocate; hot reload runs every frame in a development build.
    Array<SourceId> changed_;
};

}  // namespace cy::shader

#endif  // CY_SHADER_HOT_RELOAD_H
