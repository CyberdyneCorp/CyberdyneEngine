#pragma once
// Reading the breadcrumb ring back, for the suites that assert a phase boundary was reached.
//
// `diagnostics-profiling-and-crash` — "Breadcrumbs": "the engine SHALL record breadcrumbs at coarse
// phase boundaries: tick, stage, asset activation, level transition, and save". Those five sites
// live in five different modules, so the claim is asserted in five different suites and the two
// helpers they need are here rather than copied into each of them.
//
// WHY A MARK. The ring is one process-wide array of sixty-four slots that no drain empties — that
// is the whole point of it, and it means every case in a test binary writes into the same ring as
// every case before it. A case therefore takes a mark first and asks only about what came after,
// so an assertion cannot be satisfied by a breadcrumb an earlier case left behind.
//
// HEADER-ONLY, AND NOT A DEPENDENCY OF THE HARNESS. `cy::test-harness` does not link
// cy::core-diagnostics; a suite that includes this header links it itself, which it must do anyway
// to call `breadcrumb_snapshot`. So including this file costs nothing to the suites that do not.

#include <cy/core/diagnostics/breadcrumb.h>
#include <cy/core/diagnostics/field.h>

#include <string_view>

namespace cy::test {

/// The newest breadcrumb sequence number, or zero when the ring is empty.
[[nodiscard]] inline u64 breadcrumb_mark() noexcept {
    diag::Breadcrumb crumbs[diag::kBreadcrumbCapacity];
    const u32 count = diag::breadcrumb_snapshot(crumbs, diag::kBreadcrumbCapacity);
    return (count == 0) ? 0 : crumbs[count - 1].sequence;
}

/// How many breadcrumbs recorded after `mark` carry `phase`. Zero is the answer a boundary with no
/// caller gives, which is the state M9 shipped in and `m9:breadcrumbs-adopted` recorded.
[[nodiscard]] inline u32 breadcrumbs_since(u64 mark, std::string_view phase) noexcept {
    diag::Breadcrumb crumbs[diag::kBreadcrumbCapacity];
    const u32 count = diag::breadcrumb_snapshot(crumbs, diag::kBreadcrumbCapacity);
    u32 seen = 0;
    for (u32 index = 0; index < count; ++index) {
        if (crumbs[index].sequence <= mark) {
            continue;
        }
        const char* name = diag::lookup_name(crumbs[index].phase);
        if (name != nullptr && std::string_view(name) == phase) {
            ++seen;
        }
    }
    return seen;
}

/// The detail carried by the last breadcrumb after `mark` whose phase is `phase`, and whether there
/// was one. The detail is what makes a marker worth reading in an artefact — the tick, the cell,
/// the asset — so a suite that asserts the boundary was reached also asserts what it said.
[[nodiscard]] inline bool breadcrumb_detail_since(u64 mark, std::string_view phase,
                                                  u64& detail) noexcept {
    diag::Breadcrumb crumbs[diag::kBreadcrumbCapacity];
    const u32 count = diag::breadcrumb_snapshot(crumbs, diag::kBreadcrumbCapacity);
    bool found = false;
    for (u32 index = 0; index < count; ++index) {
        if (crumbs[index].sequence <= mark) {
            continue;
        }
        const char* name = diag::lookup_name(crumbs[index].phase);
        if (name != nullptr && std::string_view(name) == phase) {
            detail = crumbs[index].detail;
            found = true;
        }
    }
    return found;
}

}  // namespace cy::test
