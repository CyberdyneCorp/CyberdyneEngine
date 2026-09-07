#ifndef CY_IMPORT_PIPELINE_H
#define CY_IMPORT_PIPELINE_H
// The import pipeline: what actually runs an importer, and everything around it. M5 task 5.1.
//
// It joins the four pieces that are useless separately — the registry, the derived-data cache, the
// import record and the cooked output — into the operation `asset-import-pipeline` describes:
//
//   1. digest the source and build the derivation key from it, the options, the importer's version,
//      the variant and the cook profile;
//   2. ask the cache, and take a hit;
//   3. on a miss, run the importer through a resolver that records what it reads;
//   4. bind each produced sub-asset to the id it already had, or mint one;
//   5. store the result in the cache, write the cooked assets, and rewrite the two sidecars;
//   6. record what happened, with the reason, in the report.
//
// --- WHY A CACHE HIT REPRODUCES THE OUTPUT RATHER THAN SKIPPING IT
// --------------------------------
//
// A hit does not mean "there is nothing to do": it means the EXPENSIVE part is done. The cooked
// files still have to exist on disk, because a developer who deleted their cooked output directory
// and kept their cache must get their content back. So a hit decodes the cached bundle and writes
// the same files a miss would have written, and the two paths are indistinguishable from the
// outside — which is what makes "deleting the cache loses nothing" and "deleting the output loses
// nothing" both true.
//
// --- WHERE THE PARALLELISM IS, AND WHERE IT IS NOT -----------------------------------------------
//
// "Import SHALL run in parallel on the job system, ordered by dependency, with progress reporting
// and cancellation."
//
// `import_all` runs in three phases. Digesting sources, asking the cache and writing results are
// SERIAL; only the importers themselves run on the job system. That is deliberate and it is where
// the time is: an importer is seconds of mesh processing and a cache lookup is a file open. It is
// also what keeps `DerivedCache` — which is documented as not thread-safe, because a lock inside it
// would serialise every producer on one mutex — reachable from one thread only.
//
// Cancellation is checked at PHASE boundaries and between assets, never inside an importer. "WHEN
// the user cancels an import THEN it SHALL stop at the next step boundary, leaving the cache
// consistent": a cancel that tore an importer down mid-write is exactly the half-written cache
// entry the requirement is about.

#include <cy/core/assets/derived_cache.h>
#include <cy/core/assets/identity.h>
#include <cy/core/assets/path.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/import/importer.h>
#include <cy/import/report.h>
#include <cy/import/sidecar.h>

#include <atomic>
#include <string_view>

namespace cy::jobs {
class JobSystem;
}  // namespace cy::jobs

namespace cy::import {

/// The one place a source is read and the reading is recorded.
///
/// Resolves a reference relative to the source's own directory first and against the project root
/// second, which is the order every content pipeline uses and the order an artist expects. Every
/// successful read files a dependency on the result under the reference's PROJECT-RELATIVE path, so
/// two assets that read the same include record the same dependency name and the cache's
/// invalidation check does one digest for both.
class FileImportResolver final : public ImportResolver {
public:
    FileImportResolver(std::string_view project_root, const assets::VirtualPath& source,
                       ImportResult& result) noexcept;

    [[nodiscard]] Expected<Span<const u8>, Error> read(std::string_view path) noexcept override;
    [[nodiscard]] Status observe(std::string_view name,
                                 const assets::ContentHash& digest) noexcept override;

private:
    /// Every buffer this resolver has read, kept alive because the spans it handed out point into
    /// them. An importer holds a reference to a buffer for as long as it is parsing, and the
    /// alternative — handing back a copy per read — would double a large mesh buffer.
    struct Buffer {
        assets::VirtualPath path;
        Array<u8> bytes;
    };

    std::string_view project_root_;
    assets::VirtualPath directory_;
    ImportResult* result_ = nullptr;
    Array<Buffer> buffers_;
};

/// What one asset's import is asked for.
struct ImportSettings {
    assets::VariantKey variant;
    CookProfile profile = CookProfile::Client;
    /// The options to import with. Null takes the options the record already holds, which is what a
    /// re-import does; a caller that is changing an option supplies the new set.
    const ImportOptions* options = nullptr;
    /// Force a re-import even on a cache hit. For diagnosing the cache itself, and for a gate that
    /// wants to prove a cold cook works.
    bool ignore_cache = false;
    /// Whether this cook may invent an asset identity. M6 task 8.3, design.md §1.7.
    ///
    /// `Mint` is the editor's and a first import's. `Refuse` is a shipping cook's and continuous
    /// integration's: it fails naming the asset rather than drawing 128 random bits that make two
    /// cold builds of one project produce different bytes. See `MintPolicy` in importer.h.
    MintPolicy minting = MintPolicy::Mint;
};

/// Runs importers, and is the only thing that writes to the project.
class ImportPipeline {
public:
    ImportPipeline(ImporterRegistry& registry, assets::DerivedCache& cache) noexcept;

    ImportPipeline(const ImportPipeline&) = delete;
    ImportPipeline& operator=(const ImportPipeline&) = delete;

    /// Where the project's sources live, as a native filesystem path. Every `VirtualPath` this
    /// pipeline is given is resolved against it.
    [[nodiscard]] Status set_project_root(std::string_view path) noexcept;

    /// Where cooked assets are written, as a native filesystem path. Created if it is not there.
    [[nodiscard]] Status set_output_directory(std::string_view path) noexcept;

    /// Import one source. Never fails for a reason that is about the SOURCE — that is a diagnostic
    /// on the outcome — and fails only for a reason that is about the machine or the pipeline.
    [[nodiscard]] Expected<AssetImportOutcome, Error> import_file(
        const assets::VirtualPath& source, const ImportSettings& settings) noexcept;

    /// Import several sources, running the importers themselves on `jobs` when one is supplied.
    ///
    /// Returns the number imported, which is less than `sources.size()` when the run was cancelled.
    [[nodiscard]] Expected<usize, Error> import_all(Span<const assets::VirtualPath> sources,
                                                    const ImportSettings& settings,
                                                    jobs::JobSystem* job_system) noexcept;

    /// Ask the run to stop at the next boundary. Safe to call from another thread.
    void request_cancellation() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool is_cancelled() const noexcept {
        return cancelled_.load(std::memory_order_relaxed);
    }
    void reset_cancellation() noexcept { cancelled_.store(false, std::memory_order_relaxed); }

    [[nodiscard]] const ImportReport& report() const noexcept { return report_; }
    [[nodiscard]] ImportReport& report() noexcept { return report_; }

    /// The asset database this pipeline registers identities in, so a caller can resolve a
    /// reference by id after a run.
    [[nodiscard]] assets::AssetDatabase& database() noexcept { return database_; }

    /// Progress: how many of the current `import_all` have settled.
    [[nodiscard]] usize completed() const noexcept {
        return completed_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] usize total() const noexcept { return total_.load(std::memory_order_relaxed); }

private:
    struct Prepared;

    /// Steps 1 and 2: digest, key, and ask the cache.
    [[nodiscard]] Status prepare(const assets::VirtualPath& source, const ImportSettings& settings,
                                 Prepared& out) noexcept;
    /// Step 5, for both paths: write the cooked files and the two sidecars.
    [[nodiscard]] Status publish(Prepared& prepared, const ImportResult& result) noexcept;

    ImporterRegistry* registry_ = nullptr;
    assets::DerivedCache* cache_ = nullptr;
    assets::AssetDatabase database_;
    ImportReport report_;
    char project_root_[assets::kMaxPathLength + 1] = {};
    char output_directory_[assets::kMaxPathLength + 1] = {};
    std::atomic<bool> cancelled_{false};
    std::atomic<usize> completed_{0};
    std::atomic<usize> total_{0};
};

/// Register the importers this build ships: glTF, FBX and texture.
///
/// A separate function rather than a constructor's body, because `asset-import-pipeline` requires a
/// project to be able to register its own importers with the same weight as a built-in — so the
/// registry is built by a caller who chooses what goes in it, and the built-ins are one call among
/// several rather than a privileged set.
[[nodiscard]] Status register_builtin_importers(ImporterRegistry& registry) noexcept;

}  // namespace cy::import

#endif  // CY_IMPORT_PIPELINE_H
