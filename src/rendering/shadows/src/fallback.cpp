#include <cy/rendering/shadows/fallback.h>

#include <cy/core/math/scalar.h>

namespace cy::rendering {

const char* shadow_substitution_name(ShadowSubstitution substitution) noexcept {
    switch (substitution) {
        case ShadowSubstitution::Requested:
            return "Requested";
        case ShadowSubstitution::CoarserPage:
            return "CoarserPage";
        case ShadowSubstitution::StalePage:
            return "StalePage";
        case ShadowSubstitution::Approximation:
            return "Approximation";
        case ShadowSubstitution::Unshadowed:
            return "Unshadowed";
        case ShadowSubstitution::Count:
            break;
    }
    return "Unknown";
}

namespace {

/// A page is usable as-is when it holds a rendered result and nothing has dirtied it.
[[nodiscard]] bool clean_and_resident(const PageEntry* entry) noexcept {
    return entry != nullptr && entry->state == PageState::Resident && !entry->dirty;
}

/// The same page at one coarser level. The x and y halve with the level because the levels share a
/// lattice — which is the property `clipmap.h`'s snapping exists to guarantee.
[[nodiscard]] VirtualPage coarser(VirtualPage page) noexcept {
    VirtualPage up = page;
    up.level = static_cast<u8>(page.level + 1U);
    up.x = static_cast<u16>(page.x / 2U);
    up.y = static_cast<u16>(page.y / 2U);
    return up;
}

}  // namespace

FallbackResult resolve_shadow_lookup(const ShadowPageCache& cache, u64 frame_index,
                                     VirtualPage requested,
                                     const FallbackOptions& options) noexcept {
    FallbackResult result;
    result.page = requested;

    // 1. The requested page.
    const PageEntry* entry = cache.inspect(requested);
    if (clean_and_resident(entry)) {
        result.substitution = ShadowSubstitution::Requested;
        result.physical_slot = entry->physical_slot;
        return result;
    }

    // 2. A coarser page of the same light. Walks up towards the pinned tail level, which is the
    //    rung that makes the guarantee: the tail is resident by policy, so this loop terminates in
    //    something rather than in nothing whenever the tail is within reach.
    VirtualPage climb = requested;
    for (u8 step = 0; step < options.coarser_levels; ++step) {
        climb = coarser(climb);
        if (climb.level > 15U) {
            break;
        }
        const PageEntry* coarse = cache.inspect(climb);
        if (clean_and_resident(coarse)) {
            result.substitution = ShadowSubstitution::CoarserPage;
            result.page = climb;
            result.physical_slot = coarse->physical_slot;
            result.coarser = true;
            return result;
        }
        if (climb.level >= options.tail_level && options.tail_level != 0) {
            break;
        }
    }

    // 3. The requested page's previous contents, while a render is in flight or while it is dirty.
    //    Nothing is waited for here — that is the requirement, and it is why this is a read of a
    //    state rather than a fence on one.
    if (entry != nullptr && entry->state != PageState::Absent) {
        const u64 age =
            frame_index > entry->rendered_frame ? frame_index - entry->rendered_frame : 0;
        const bool fresh_enough =
            options.max_stale_frames == 0 || age <= static_cast<u64>(options.max_stale_frames);
        if (fresh_enough) {
            result.substitution = ShadowSubstitution::StalePage;
            result.physical_slot = entry->physical_slot;
            return result;
        }
    }

    // 4. A screen-space or traced approximation.
    if (options.approximation_available) {
        result.substitution = ShadowSubstitution::Approximation;
        return result;
    }

    // 5. Unshadowed. Counted loudly by the ledger: a light silently losing its shadow is the
    //    failure the whole chain exists to make visible.
    result.substitution = ShadowSubstitution::Unshadowed;
    return result;
}

u8 shadow_critical_level(const ShadowPageGeometry& geometry) noexcept {
    // The level at which the whole light fits in one page. Derived, so a project cannot pin a level
    // so fine that the guarantee stops being one: at this level there is exactly one page per face,
    // and one page per face is what "guaranteed resident" can afford to mean.
    u32 side = geometry.pages_per_side();
    u8 level = 0;
    while (side > 1U && level < 15U) {
        side /= 2U;
        ++level;
    }
    return level;
}

void SubstitutionLedger::record(ShadowSubstitution substitution) noexcept {
    const auto index = static_cast<usize>(substitution);
    if (index < static_cast<usize>(ShadowSubstitution::Count)) {
        ++counts[index];
    }
}

void SubstitutionLedger::reset() noexcept {
    for (u32& count : counts) {
        count = 0;
    }
}

u32 SubstitutionLedger::total() const noexcept {
    u32 sum = 0;
    for (const u32 count : counts) {
        sum += count;
    }
    return sum;
}

u32 SubstitutionLedger::substituted() const noexcept {
    return total() - counts[static_cast<usize>(ShadowSubstitution::Requested)];
}

}  // namespace cy::rendering
