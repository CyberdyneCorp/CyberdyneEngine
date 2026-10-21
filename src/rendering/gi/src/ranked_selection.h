// SPDX-License-Identifier: MIT
#pragma once
// The budgeted selection both caches schedule with, in time linear in the cache rather than
// `n log n`. Private to the module: `SurfaceCache::select` and `RadianceCache::select` are the
// public functions, and their contract is unchanged.
//
// ================================================================================================
// WHY IT IS THE SAME SELECTION
// ================================================================================================
//
// Both selections were a full sort of every live entry by (starved first, priority descending,
// handle ascending), cut to the budget. That order is TOTAL — the handle breaks every tie — so the
// first `limit` entries of the sorted array are one set in one order however they are found. A
// partition about the `limit`-th (`std::nth_element`) followed by a sort of the prefix finds them
// in O(n + k log k), where the full sort was O(n log n) with the priority recomputed at every
// comparison. A game's caches hold tens of thousands of entries and the budget is a few hundred.
// The rank keys are computed once per entry by the caller's own priority function, so every
// comparison sees the float the sort saw.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

#include <algorithm>

namespace cy::rendering::gi::detail {

/// One live entry's place in the schedule.
struct RankedEntry {
    f32 priority = 0.0F;
    u32 handle = 0;
    /// Older than the progress guarantee's cap: ranks ahead of every entry that is not.
    bool starved = false;
};

/// The schedule's order: starved first, then the higher priority, then the lower handle.
[[nodiscard]] inline bool ranks_before(const RankedEntry& a, const RankedEntry& b) noexcept {
    if (a.starved != b.starved) {
        return a.starved;
    }
    if (a.priority != b.priority) {
        return a.priority > b.priority;
    }
    return a.handle < b.handle;
}

/// Order the first `limit` entries of `ranked` as a full sort would and copy their handles into
/// `selected`. The entries past `limit` are left in `ranked[limit..]`, in no particular order: the
/// ones the update does not service.
[[nodiscard]] inline Status take_ranked(Array<RankedEntry>& ranked, usize limit,
                                        Array<u32>& selected) noexcept {
    const usize count = std::min(limit, ranked.size());
    RankedEntry* first = ranked.data();
    if (count < ranked.size()) {
        std::nth_element(first, first + count, first + ranked.size(), ranks_before);
    }
    std::sort(first, first + count, ranks_before);
    selected.clear();
    if (Status sized = selected.resize(count); !sized) {
        return sized;
    }
    for (usize index = 0; index < count; ++index) {
        selected[index] = ranked[index].handle;
    }
    return ok();
}

}  // namespace cy::rendering::gi::detail
