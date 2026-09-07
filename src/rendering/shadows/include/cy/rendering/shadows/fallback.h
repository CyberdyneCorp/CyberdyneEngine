#pragma once
// The fallback chain, and the record of which rung was taken. Task 8.2.
//
// `virtual-shadows` — "Fallback chain": "When a required shadow page is unavailable, the system
// SHALL degrade along a defined chain and SHALL NOT stall: the requested page, then a coarser page,
// then a stale cached page, then a screen-space or traced approximation, then unshadowed."
//
// ================================================================================================
// THE FRAME NEVER WAITS, WHICH IS WHY THIS IS A FUNCTION AND NOT A FENCE
// ================================================================================================
//
// "The frame SHALL never wait for shadow page production." There is nothing in this file that can
// block: `resolve_shadow_lookup()` walks the rungs, returns the first one that is available this
// instant, and records it. A page whose render is in flight is not waited for — its previous
// contents are still sampleable, and that is the `StalePage` rung.
//
// The same argument settles the cycle between shadows and virtual textures. Shadow rasterisation
// needs opacity, opacity may be virtualised, and virtual texture residency is driven by visibility
// which depends on shadows. `shadow_critical_level()` is this module's half: a shadow program's
// textures declare a coarse level that is pinned resident, rasterisation proceeds with it when a
// finer page is missing, and the page is marked for refresh. Nothing awaits a texture either.
//
// ================================================================================================
// THE SUBSTITUTION IS RECORDED BECAUSE OTHERWISE SOFTNESS IS A MYSTERY
// ================================================================================================
//
// "The substitution used SHALL be recorded per page and reportable, so that unexpected softness or
// missing shadow is diagnosable rather than mysterious." A shadow that is one clip level too coarse
// looks like a bias problem, a filtering problem, or a content problem, and it is none of them.
// `SubstitutionLedger` counts each rung per frame; the per-page record is `PageEntry` in `cache.h`
// plus the rung this function returned.

#include <cy/core/base/types.h>
#include <cy/rendering/shadows/cache.h>

namespace cy::rendering {

/// The chain, in the order the specification defines it. Ordered so that a smaller enumerator is a
/// better answer, which is what makes "the first rung that is available" a loop and not a table.
enum class ShadowSubstitution : u8 {
    /// The requested page, resident and clean. No substitution.
    Requested = 0,
    /// A coarser page of the same light, resident.
    CoarserPage,
    /// The requested page's previous contents, while a render is in flight or while it is dirty.
    StalePage,
    /// A screen-space or traced approximation.
    Approximation,
    /// Nothing. The light contributes unshadowed, and this is counted loudly.
    Unshadowed,
    Count,
};

[[nodiscard]] const char* shadow_substitution_name(ShadowSubstitution substitution) noexcept;

struct FallbackOptions {
    /// How many coarser levels the walk may climb before it gives up on the paged result.
    u8 coarser_levels = 4;
    /// The coarsest level of this light's space — the level that is pinned resident, the shadow
    /// equivalent of a virtual texture's mip tail.
    u8 tail_level = 0;
    /// A screen-space or traced approximation is available for this pixel this frame.
    bool approximation_available = false;
    /// A page whose age exceeds this is treated as too stale to substitute, and the walk continues
    /// to the approximation. Zero means any stale page will do.
    u32 max_stale_frames = 0;
};

struct FallbackResult {
    ShadowSubstitution substitution = ShadowSubstitution::Unshadowed;
    /// The page actually sampled. Meaningless when the rung is `Approximation` or `Unshadowed`.
    VirtualPage page;
    u32 physical_slot = 0xFFFFFFFFU;
    /// True when the answer came from a level coarser than the one asked for; the number of levels
    /// climbed is `page.level - requested.level`.
    bool coarser = false;
};

/// Walk the chain for one lookup. Reads the cache and changes nothing in it — a lookup is a
/// shader's question, and a function that dirtied a page while answering it would make the shadow
/// cost depend on how many pixels asked.
[[nodiscard]] FallbackResult resolve_shadow_lookup(const ShadowPageCache& cache, u64 frame_index,
                                                   VirtualPage requested,
                                                   const FallbackOptions& options) noexcept;

/// The coarse level that must be resident before a shadow program samples a virtual texture: the
/// "shadow-critical" guarantee. Derived from the page geometry rather than configured, so that a
/// project cannot pin a level so fine that the guarantee stops being one.
[[nodiscard]] u8 shadow_critical_level(const ShadowPageGeometry& geometry) noexcept;

struct SubstitutionLedger {
    u32 counts[static_cast<usize>(ShadowSubstitution::Count)] = {};

    void record(ShadowSubstitution substitution) noexcept;
    void reset() noexcept;

    [[nodiscard]] u32 total() const noexcept;
    /// Lookups that got anything other than the page they asked for.
    [[nodiscard]] u32 substituted() const noexcept;
};

}  // namespace cy::rendering
