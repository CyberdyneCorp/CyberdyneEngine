#ifndef CY_IMPORT_REPORT_H
#define CY_IMPORT_REPORT_H
// What an import run has to say for itself. M5 task 5.1.
//
// `asset-import-pipeline` — "Import diagnostics": "The pipeline SHALL report per asset: import
// duration, output size, format chosen, warnings and errors, and the cache outcome (hit, miss, or
// invalidated with the reason). A project-level report SHALL summarise: total cooked size by
// category, the largest assets, assets with warnings, and assets whose import is slowest."
//
// --- WHY THE CACHE OUTCOME IS IN THE PER-ASSET ROW, NOT ONLY IN A TOTAL --------------------------
//
// "Why is this re-cooking every time" is the question a content pipeline gets asked most, and it is
// unanswerable from a total. The row carries the outcome AND the reason — `invalidated:
// shaders/common.slang changed` — because the reason is what turns a complaint into a fix. This is
// the same argument `derived_cache.h` makes for `CacheResult::stale` naming the dependency: a cache
// that says "miss" teaches nothing.

#include <cy/core/assets/derived_cache.h>
#include <cy/core/assets/identity.h>
#include <cy/core/assets/path.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/import/importer.h>

#include <string_view>

namespace cy::import {

/// One asset's row in the report.
///
/// Every field is a number or an inline string: the report outlives the import it describes, and a
/// row that pointed at an importer's buffers would dangle the moment the run moved on.
struct AssetImportOutcome {
    static constexpr usize kNameCapacity = 192;

    /// The source, project-relative.
    char source[kNameCapacity] = {};
    /// Which importer ran, or which one would have.
    char importer[64] = {};
    /// The identity the source holds. Nil when the import failed before one was assigned.
    cy::AssetId id;

    assets::CacheOutcome cache = assets::CacheOutcome::Miss;
    assets::CacheTier tier = assets::CacheTier::None;
    /// Why the cache said what it did. Never null.
    char cache_reason[kNameCapacity] = {};

    usize sub_assets = 0;
    usize warnings = 0;
    usize errors = 0;
    /// The cooked bytes written, across every sub-asset.
    usize cooked_bytes = 0;
    /// How many sub-asset ids had to be minted. Zero on a re-import of an unchanged source, which
    /// is the assertion that "references survive re-import" is worth making.
    usize minted_ids = 0;
    /// How many bindings the record dropped because the source no longer produces them.
    usize orphaned_ids = 0;
    /// How many sub-assets the cook profile excluded, and what they would have cost. M6 task 8.3.
    ///
    /// `asset-import-pipeline` — "Each cook SHALL report what was excluded and the resulting size
    /// by category, so accidental inclusions are visible." A count alone would not do it: the
    /// scenario is "a server cook unexpectedly includes a large texture", and it is the BYTES that
    /// make that visible.
    usize excluded_sub_assets = 0;
    usize excluded_bytes = 0;
    /// Which profile this row was cooked under, so a report over a mixed run is readable.
    CookProfile profile = CookProfile::Client;
    u64 duration_micros = 0;

    [[nodiscard]] bool succeeded() const noexcept { return errors == 0; }
};

/// Everything a run has to say.
///
/// Accumulating rather than streaming, because every summary the requirement names — the largest
/// assets, the slowest importers, the ones with warnings — is a question about the whole run.
class ImportReport {
public:
    ImportReport() noexcept = default;

    ImportReport(const ImportReport&) = delete;
    ImportReport& operator=(const ImportReport&) = delete;

    [[nodiscard]] Status add(const AssetImportOutcome& outcome) noexcept;

    [[nodiscard]] Span<const AssetImportOutcome> rows() const noexcept;
    [[nodiscard]] usize size() const noexcept { return rows_.size(); }
    void clear() noexcept { rows_.clear(); }

    [[nodiscard]] usize total_warnings() const noexcept;
    [[nodiscard]] usize total_errors() const noexcept;
    [[nodiscard]] usize total_cooked_bytes() const noexcept;
    /// What the cook profile removed, across the run. Zero for a `Client` or `Editor` cook, which
    /// exclude nothing.
    [[nodiscard]] usize total_excluded_sub_assets() const noexcept;
    [[nodiscard]] usize total_excluded_bytes() const noexcept;
    [[nodiscard]] usize cache_hits() const noexcept;
    [[nodiscard]] usize cache_misses() const noexcept;
    [[nodiscard]] usize invalidations() const noexcept;
    [[nodiscard]] u64 total_micros() const noexcept;

    /// The `count` largest rows by cooked size, most first, written into `out`. Returns how many
    /// were written, which is the smaller of `count` and the number of rows.
    [[nodiscard]] usize largest(usize count, Span<const AssetImportOutcome*> out) const noexcept;

    /// The `count` slowest rows, slowest first. Same shape as `largest`.
    [[nodiscard]] usize slowest(usize count, Span<const AssetImportOutcome*> out) const noexcept;

    /// Render a human-readable summary: the totals, the cache's behaviour, then the largest and
    /// slowest, then every row that had something to say.
    ///
    /// Writes at most `capacity` bytes including the terminator and reports how many it wrote.
    /// Truncates rather than failing — a report is a diagnostic and half of one is better than an
    /// error about the buffer — and says so on the last line when it does.
    [[nodiscard]] usize format(char* out, usize capacity) const noexcept;

private:
    Array<AssetImportOutcome> rows_;
};

}  // namespace cy::import

#endif  // CY_IMPORT_REPORT_H
