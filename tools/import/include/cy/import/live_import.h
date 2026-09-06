#ifndef CY_IMPORT_LIVE_IMPORT_H
#define CY_IMPORT_LIVE_IMPORT_H
// Live asset reload: an edited source becomes a re-cook becomes a reload, without a restart. M5
// task 5.2.
//
// `live-editing` — "Live asset reload": "Assets SHALL be live-reloadable: recompiled or reimported
// content SHALL replace the resource behind an existing **stable handle**, so holders need not
// re-resolve ... A failed reload SHALL keep the previous resource and report the failure."
//
// And "Live editing is a compilation step": "Changes made in the editor SHALL reach a running world
// through a **live edit compiler** that translates an authoring delta into a validated runtime
// delta." For an ASSET, that compiler is the import pipeline: the delta is the file that changed,
// the compilation is the import, and the validation is the importer's own diagnostics. This class
// is the loop that joins the three, and it deliberately adds no second path — everything goes
// through `ImportPipeline` and `AssetSystem::reload`, which is what makes a live reload and a cold
// cook produce the same bytes.
//
// --- WHAT THIS OWNS, AND WHAT IT DELIBERATELY DOES NOT -------------------------------------------
//
// It owns the ASSET half of live editing. The rest of `live-editing` — the per-field live edit
// policy, entity and component deltas, play modes and the bridge's message set — is not here and is
// not this module's: a policy is a property of a reflected field and belongs with the field
// classification, and the bridge is the editor's protocol crate and the hosted runtime. What is
// here is the half that has a stable handle to replace and a compiler to run, and it is complete
// for that half.
//
// --- WHY IT RE-IMPORTS EVERY REGISTERED SOURCE RATHER THAN THE ONE THAT CHANGED ------------------
//
// Because the file that changed is very often not the asset that has to be re-cooked. A glTF's
// external `.bin`, a shader's include, a texture a material references: none of them is an asset
// the registry claims, and all of them invalidate something. Maintaining a reverse index from
// dependency to dependent would be a second copy of the knowledge the cache already holds — it
// records what each entry read, and `DerivedCache::lookup` re-digests it — so this asks the
// pipeline instead, and the cache answers "hit" for everything unaffected.
//
// The cost is one cache lookup per registered source per change event, which is a file open each.
// At the scale a live session works on — the assets somebody is iterating on, not the whole project
// — that is the right trade. A session that registered ten thousand sources would want the reverse
// index, and the place to put it is `DerivedCache`, which is where the dependencies already are.
//
// --- THE SIDECARS ARE UNDER THE WATCHED TREE, AND THAT SETTLES ----------------------------------
//
// An import rewrites `<source>.meta` and `<source>.import`, which are beside the source and are
// therefore watched. So a real change produces one extra poll that sees the sidecars move and
// re-imports — every source a cache hit, nothing re-cooked — and then stops, because the watcher
// fingerprints CONTENT and a sidecar rewritten with the same bytes is not a change. The
// alternative, excluding the sidecars from the watch, would mean an edit made by hand to an import
// option was never noticed, which is a worse failure than one extra sweep.

#include <cy/core/assets/asset_system.h>
#include <cy/core/assets/path.h>
#include <cy/core/assets/watch.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/asset_id.h>
#include <cy/import/pipeline.h>

#include <string_view>

namespace cy::import {

/// What happened to one live edit.
///
/// `live-editing` — "Live editing diagnostics": "When a live edit does not visibly take effect, the
/// developer SHALL be able to determine whether it was refused, deferred, applied to a different
/// instance set, or overwritten by simulation." These are those answers, for an asset.
enum class LiveEditOutcome : u8 {
    /// Re-cooked, and the resident asset's bytes were replaced behind the handles that hold it.
    Applied = 0,
    /// Re-cooked, and nothing was resident to replace. The next load takes the new bytes.
    NotResident = 1,
    /// The cache answered, so nothing about this asset changed. The commonest outcome by far, and
    /// the reason a change to one file does not re-cook a project.
    Unchanged = 2,
    /// The importer refused it: a malformed source, an option it cannot honour. The previous asset
    /// is untouched.
    Refused = 3,
    /// The re-cook succeeded and the reload did not. The previous bytes are still in use, which is
    /// the specification's requirement and not a consolation.
    ReloadFailed = 4,
};

/// The enumerator's own spelling. Never null.
[[nodiscard]] const char* live_edit_outcome_name(LiveEditOutcome outcome) noexcept;

/// One line of the live edit log.
struct LiveEditRecord {
    static constexpr usize kNameCapacity = 192;

    char source[kNameCapacity] = {};
    LiveEditOutcome outcome = LiveEditOutcome::Unchanged;
    /// Why, in one line. Never null, and empty only when there is nothing to say.
    char reason[kNameCapacity] = {};
    /// The asset the source's primary sub-asset holds.
    cy::AssetId id;
    /// How many resident assets had their bytes replaced.
    u32 reloaded = 0;
    u64 duration_micros = 0;
};

/// How the session has behaved, for the diagnostic `live-editing` requires.
struct LiveEditStats {
    u64 polls = 0;
    u64 changes_seen = 0;
    u64 recooked = 0;
    u64 applied = 0;
    u64 refused = 0;
    u64 reload_failures = 0;
    /// The wall-clock cost of the last poll that did any work — "live edit latency", which the
    /// specification names among the things to report.
    u64 last_latency_micros = 0;
};

/// Watches sources, re-imports what a change affected, and reloads what is resident.
///
/// Not thread-safe, by the same rule as `FileWatcher` and `AssetSystem::reload`: `poll` reads files
/// and must be called from a thread where blocking is legal — an editor tick or a tool's loop,
/// never a job worker.
class LiveImportSession {
public:
    LiveImportSession(ImportPipeline& pipeline, assets::FileWatcher& watcher) noexcept;

    LiveImportSession(const LiveImportSession&) = delete;
    LiveImportSession& operator=(const LiveImportSession&) = delete;

    /// The asset system whose resident assets are replaced. Optional: a cooker has none, and a
    /// session with none still re-cooks and reports `NotResident`.
    void bind(assets::AssetSystem* system) noexcept { assets_ = system; }

    /// Keep a source live. Registering the same source twice registers it once.
    [[nodiscard]] Status register_source(const assets::VirtualPath& source) noexcept;

    /// Watch a file or a directory for change. Passed straight to the watcher, whose recursive
    /// directory watch is what lets a session say "everything under content/" in one call.
    [[nodiscard]] Status watch(const assets::VirtualPath& path) noexcept;

    /// Take the watcher's baseline without reporting anything, so that opening a project does not
    /// re-cook every file in it.
    [[nodiscard]] Status prime(i64 now_ns) noexcept;

    /// One turn of the loop: poll the watcher, and if anything changed, re-import every registered
    /// source and reload what the pipeline replaced.
    ///
    /// Returns how many sources were actually re-cooked, which is zero on the overwhelming majority
    /// of polls — nothing changed — and small when something did.
    [[nodiscard]] Expected<usize, Error> poll(i64 now_ns, const ImportSettings& settings) noexcept;

    /// What the last `poll` did, one record per registered source that was not `Unchanged`.
    [[nodiscard]] Span<const LiveEditRecord> last_edits() const noexcept;

    [[nodiscard]] const LiveEditStats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = {}; }

    [[nodiscard]] usize source_count() const noexcept { return sources_.size(); }

private:
    /// The watcher's observer. Only counts: which file changed does not decide what is re-imported
    /// — see the header — so the observer's whole job is to say that something did.
    static void on_change(void* user, const assets::WatchEvent& event) noexcept;

    ImportPipeline* pipeline_ = nullptr;
    assets::FileWatcher* watcher_ = nullptr;
    assets::AssetSystem* assets_ = nullptr;
    Array<assets::VirtualPath> sources_;
    Array<LiveEditRecord> last_edits_;
    LiveEditStats stats_{};
    u32 changes_this_poll_ = 0;
};

}  // namespace cy::import

#endif  // CY_IMPORT_LIVE_IMPORT_H
