#include <cy/import/report.h>

#include <cstdio>
#include <cstring>

#include <algorithm>
#include <vector>

namespace cy::import {
namespace {

/// Append at most what fits, and say whether it all fitted. The report truncates rather than
/// failing, so every caller of this ignores the result except the last line, which reports it.
[[nodiscard]] bool append(char* out, usize capacity, usize& written,
                          std::string_view text) noexcept {
    if (written >= capacity) {
        return false;
    }
    const usize room = capacity - written - 1;
    const usize length = text.size() < room ? text.size() : room;
    if (length != 0) {
        std::memcpy(out + written, text.data(), length);
    }
    written += length;
    out[written] = '\0';
    return length == text.size();
}

}  // namespace

Status ImportReport::add(const AssetImportOutcome& outcome) noexcept {
    return rows_.push_back(outcome);
}

Span<const AssetImportOutcome> ImportReport::rows() const noexcept {
    return {rows_.data(), rows_.size()};
}

usize ImportReport::total_warnings() const noexcept {
    usize total = 0;
    for (const AssetImportOutcome& row : rows_) {
        total += row.warnings;
    }
    return total;
}

usize ImportReport::total_errors() const noexcept {
    usize total = 0;
    for (const AssetImportOutcome& row : rows_) {
        total += row.errors;
    }
    return total;
}

usize ImportReport::total_cooked_bytes() const noexcept {
    usize total = 0;
    for (const AssetImportOutcome& row : rows_) {
        total += row.cooked_bytes;
    }
    return total;
}

usize ImportReport::total_excluded_sub_assets() const noexcept {
    usize total = 0;
    for (const AssetImportOutcome& row : rows_) {
        total += row.excluded_sub_assets;
    }
    return total;
}

usize ImportReport::total_excluded_bytes() const noexcept {
    usize total = 0;
    for (const AssetImportOutcome& row : rows_) {
        total += row.excluded_bytes;
    }
    return total;
}

usize ImportReport::cache_hits() const noexcept {
    usize total = 0;
    for (const AssetImportOutcome& row : rows_) {
        total += row.cache == assets::CacheOutcome::Hit ? 1U : 0U;
    }
    return total;
}

usize ImportReport::cache_misses() const noexcept {
    usize total = 0;
    for (const AssetImportOutcome& row : rows_) {
        total += row.cache == assets::CacheOutcome::Miss ? 1U : 0U;
    }
    return total;
}

usize ImportReport::invalidations() const noexcept {
    usize total = 0;
    for (const AssetImportOutcome& row : rows_) {
        total += row.cache == assets::CacheOutcome::Invalidated ? 1U : 0U;
    }
    return total;
}

u64 ImportReport::total_micros() const noexcept {
    u64 total = 0;
    for (const AssetImportOutcome& row : rows_) {
        total += row.duration_micros;
    }
    return total;
}

namespace {

/// The `count` rows an ordering puts first, written into `out`.
///
/// One function for both rankings rather than two nearly identical ones, because the only thing
/// that differs is the comparison — and a second copy of a sort is a second place for a tie-break
/// to diverge, which would make two runs of the same import report different "largest" lists.
template <class Less>
[[nodiscard]] usize ranked(Span<const AssetImportOutcome> rows, usize count,
                           Span<const AssetImportOutcome*> out, Less less) noexcept {
    std::vector<const AssetImportOutcome*> pointers;
    pointers.reserve(rows.size());
    for (const AssetImportOutcome& row : rows) {
        pointers.push_back(&row);
    }
    std::ranges::stable_sort(pointers, less);
    const usize produced = std::min({count, out.size(), pointers.size()});
    for (usize index = 0; index < produced; ++index) {
        out[index] = pointers[index];
    }
    return produced;
}

}  // namespace

usize ImportReport::largest(usize count, Span<const AssetImportOutcome*> out) const noexcept {
    return ranked(rows(), count, out,
                  [](const AssetImportOutcome* a, const AssetImportOutcome* b) noexcept {
                      if (a->cooked_bytes != b->cooked_bytes) {
                          return a->cooked_bytes > b->cooked_bytes;
                      }
                      // A total tie-break by name, so the list is the same on two machines. A
                      // stable sort alone would only preserve the order rows arrived in, and a
                      // parallel run does not settle them in a fixed order.
                      return std::string_view(a->source) < std::string_view(b->source);
                  });
}

usize ImportReport::slowest(usize count, Span<const AssetImportOutcome*> out) const noexcept {
    return ranked(rows(), count, out,
                  [](const AssetImportOutcome* a, const AssetImportOutcome* b) noexcept {
                      if (a->duration_micros != b->duration_micros) {
                          return a->duration_micros > b->duration_micros;
                      }
                      return std::string_view(a->source) < std::string_view(b->source);
                  });
}

usize ImportReport::format(char* out, usize capacity) const noexcept {
    if (out == nullptr || capacity == 0) {
        return 0;
    }
    usize written = 0;
    out[0] = '\0';

    char line[512] = {};
    bool complete = true;

    const auto line_of = [&](const char* text) noexcept {
        complete = append(out, capacity, written, text) && complete;
    };

    (void)std::snprintf(line, sizeof(line),
                        "import: %zu asset(s), %zu warning(s), %zu error(s), %zu byte(s) cooked in "
                        "%llu ms\n",
                        rows_.size(), total_warnings(), total_errors(), total_cooked_bytes(),
                        static_cast<unsigned long long>(total_micros() / 1000));
    line_of(line);
    (void)std::snprintf(line, sizeof(line), "cache:  %zu hit, %zu miss, %zu invalidated\n",
                        cache_hits(), cache_misses(), invalidations());
    line_of(line);
    // "Each cook SHALL report what was excluded and the resulting size by category, so accidental
    // inclusions are visible." The line is printed only when a profile actually excluded something,
    // because a `Client` cook that reported "0 excluded" every time would train a reader to skip
    // the line that matters on a server cook.
    if (total_excluded_sub_assets() != 0) {
        (void)std::snprintf(line, sizeof(line),
                            "profile: %zu sub-asset(s) excluded, %zu byte(s) saved\n",
                            total_excluded_sub_assets(), total_excluded_bytes());
        line_of(line);
    }

    const AssetImportOutcome* ranked_rows[5] = {};
    const usize largest_count = largest(5, Span<const AssetImportOutcome*>(ranked_rows));
    if (largest_count != 0) {
        line_of("largest:\n");
        for (usize index = 0; index < largest_count; ++index) {
            (void)std::snprintf(line, sizeof(line), "  %10zu B  %s\n",
                                ranked_rows[index]->cooked_bytes, ranked_rows[index]->source);
            line_of(line);
        }
    }
    const usize slowest_count = slowest(5, Span<const AssetImportOutcome*>(ranked_rows));
    if (slowest_count != 0) {
        line_of("slowest:\n");
        for (usize index = 0; index < slowest_count; ++index) {
            (void)std::snprintf(
                line, sizeof(line), "  %8llu ms  %s (%s)\n",
                static_cast<unsigned long long>(ranked_rows[index]->duration_micros / 1000),
                ranked_rows[index]->source, ranked_rows[index]->importer);
            line_of(line);
        }
    }

    // Every row that had something to say. A run where nothing did prints nothing here, which is
    // the point: a report a person reads every build must be quiet when there is nothing wrong.
    bool header = false;
    for (const AssetImportOutcome& row : rows_) {
        if (row.warnings == 0 && row.errors == 0 &&
            row.cache != assets::CacheOutcome::Invalidated) {
            continue;
        }
        if (!header) {
            line_of("assets with something to report:\n");
            header = true;
        }
        (void)std::snprintf(line, sizeof(line),
                            "  %s: %zu warning(s), %zu error(s), cache %s%s%s\n", row.source,
                            row.warnings, row.errors, assets::cache_outcome_name(row.cache),
                            row.cache_reason[0] != '\0' ? " — " : "", row.cache_reason);
        line_of(line);
    }

    if (!complete) {
        // Said rather than silently dropped: a truncated report that did not admit it is how a
        // missing error becomes "the importer was fine".
        const std::string_view notice = "... report truncated\n";
        if (written + notice.size() < capacity) {
            (void)append(out, capacity, written, notice);
        }
    }
    return written;
}

}  // namespace cy::import
