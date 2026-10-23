// SPDX-License-Identifier: MIT
// The caches' budgeted selection, held to the full sort it replaced. Issue #35.
//
// `SurfaceCache::select` and `RadianceCache::select` were a sort of every live entry by the
// schedule's order, cut to the budget; they are now a partition about the budget and a sort of the
// part kept (`ranked_selection.h`), linear in the cache. The order is total, so the two must give
// the same handles in the same order, and the same report — checked here against the full sort,
// written out as it was, over caches with ties in every key the order breaks.

#include <cy/rendering/gi/radiance_cache.h>
#include <cy/rendering/gi/surface_cache.h>
#include <cy/test/test.h>

#include <algorithm>
#include <vector>

namespace gi = cy::rendering::gi;
using cy::Array;
using cy::f32;
using cy::u32;
using cy::u64;

namespace {

/// A deterministic spread of pages: some dead, some invalid, some visible, errors and ages drawn
/// from few values so priorities tie, and a few past the age cap.
[[nodiscard]] std::vector<gi::SurfacePage> pages(u32 count) {
    std::vector<gi::SurfacePage> out(count);
    u32 state = 12345U;
    const auto next = [&state]() {
        state = (state * 1664525U) + 1013904223U;
        return state >> 8U;
    };
    for (gi::SurfacePage& page : out) {
        page.live = next() % 7U != 0U;
        page.valid = next() % 3U != 0U;
        page.visible = next() % 4U == 0U;
        page.error = static_cast<f32>(next() % 4U) * 0.25F;
        page.last_update_frame = 1000U - (next() % 300U);
    }
    return out;
}

/// The selection as it was: every live page, sorted whole, cut to the budget.
[[nodiscard]] std::vector<u32> full_sort(const std::vector<gi::SurfacePage>& all, u64 frame,
                                         u64 max_age, u32 budget) {
    const auto age = [frame](const gi::SurfacePage& page) {
        return frame > page.last_update_frame ? frame - page.last_update_frame : 0;
    };
    const auto priority = [&](const gi::SurfacePage& page) {
        f32 value = 0.0F;
        value += page.visible ? 1000.0F : 0.0F;
        value += page.valid ? 0.0F : 100.0F;
        value += page.error * 10.0F;
        value += std::min(static_cast<f32>(age(page)), 1000.0F) * 0.01F;
        return value;
    };
    std::vector<u32> live;
    for (u32 handle = 0; handle < all.size(); ++handle) {
        if (all[handle].live) {
            live.push_back(handle);
        }
    }
    std::ranges::sort(live, [&](u32 a, u32 b) {
        const bool starved_a = age(all[a]) >= max_age;
        const bool starved_b = age(all[b]) >= max_age;
        if (starved_a != starved_b) {
            return starved_a;
        }
        if (priority(all[a]) != priority(all[b])) {
            return priority(all[a]) > priority(all[b]);
        }
        return a < b;
    });
    live.resize(std::min<std::size_t>(live.size(), budget));
    return live;
}

}  // namespace

CY_TEST_CASE("the surface cache's selection is the full sort's, budget for budget") {
    // THE ORACLE IS SORTED ONCE. A budget's expected selection is a prefix of the one full ranking,
    // which is what `full_sort` cuts to; sorting all three hundred pages again for each of five
    // budgets, and finding each page in the selection by a linear search, put this unit case at
    // the edge of its Debug budget on a loaded runner without asking anything more of `select`.
    const std::vector<gi::SurfacePage> all = pages(300);
    const std::vector<u32> ranking = full_sort(all, 1000, 240, static_cast<u32>(all.size()));
    u32 live = 0;
    u32 invalid = 0;
    for (const gi::SurfacePage& page : all) {
        live += page.live ? 1U : 0U;
        invalid += page.live && !page.valid ? 1U : 0U;
    }
    for (const u32 budget : {0U, 1U, 37U, 255U, 10000U}) {
        gi::SurfaceUpdateContext context;
        context.frame = 1000;
        context.max_age_frames = 240;
        context.budget = budget;
        Array<u32> selected;
        gi::SurfaceUpdateReport report;
        CY_REQUIRE(
            gi::SurfaceCache::select({all.data(), all.size()}, context, false, selected, report)
                .has_value());
        const std::size_t kept = std::min<std::size_t>(ranking.size(), budget);
        CY_REQUIRE_EQ(selected.size(), kept);
        std::vector<bool> chosen(all.size(), false);
        for (std::size_t index = 0; index < kept; ++index) {
            CY_CHECK_EQ(selected[index], ranking[index]);
            chosen[ranking[index]] = true;
        }
        // The report: the live count, the invalid count, and the oldest page left out.
        CY_CHECK_EQ(report.queue_depth, live);
        CY_CHECK_EQ(report.pages_invalid, invalid);
        u64 oldest = 0;
        for (u32 handle = 0; handle < all.size(); ++handle) {
            if (all[handle].live && !chosen[handle]) {
                oldest = std::max<u64>(oldest, 1000U - all[handle].last_update_frame);
            }
        }
        CY_CHECK_EQ(report.oldest_unserviced_age, oldest);
    }
}

CY_TEST_CASE("the radiance cache's selection takes the highest priorities in their order") {
    std::vector<gi::Probe> probes(400);
    u32 state = 777U;
    for (gi::Probe& probe : probes) {
        state = (state * 1664525U) + 1013904223U;
        probe.live = (state >> 9U) % 5U != 0U;
        probe.valid = (state >> 13U) % 2U == 0U;
        probe.error = static_cast<f32>((state >> 17U) % 3U) * 0.5F;
        probe.camera_distance = static_cast<f32>((state >> 20U) % 4U);
        probe.last_update_frame = 500U - ((state >> 5U) % 400U);
    }
    gi::ProbeUpdateContext context;
    context.frame = 500;
    context.max_age_frames = 300;
    Array<u32> all;
    gi::ProbeUpdateReport everything;
    context.converged = true;
    CY_REQUIRE(gi::RadianceCache::select({probes.data(), probes.size()}, context, all, everything)
                   .has_value());
    context.converged = false;
    for (const u32 budget : {1U, 64U, 199U}) {
        context.budget = budget;
        Array<u32> selected;
        gi::ProbeUpdateReport report;
        CY_REQUIRE(
            gi::RadianceCache::select({probes.data(), probes.size()}, context, selected, report)
                .has_value());
        // Converged mode takes every live probe in the schedule's order; a budget is its prefix.
        CY_REQUIRE_EQ(selected.size(), static_cast<std::size_t>(budget));
        for (u32 index = 0; index < budget; ++index) {
            CY_CHECK_EQ(selected[index], all[index]);
        }
        CY_CHECK_EQ(report.queue_depth, everything.queue_depth);
    }
}
