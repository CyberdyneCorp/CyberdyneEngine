#pragma once
// samples/07-fidelity — the SHADED frame, and the virtual shadow in it. M11.c task 4, and the
// criterion `m11c:virtual-geometry-image`.
//
// ================================================================================================
// WHY THIS FILE EXISTS: THE ROW'S PICTURE WAS A NORMALS DEBUG VIEW
// ================================================================================================
//
// `docs/design/images/virtual-geometry-shaded.png` was published as *"Resolved world normals under
// one light"* — one `abs()` of a fixed dot product over the resolve's normal, written by a recipe a
// person runs. Two things were wrong with it as the evidence for `virtual-geometry` and
// `virtual-shadows`: it is not a shading result, and nothing re-photographed it or compared it, so
// the row's picture was asserted by nobody.
//
// This is the shading, factored out of the capture tool so that the picture a test compares and the
// picture the documentation publishes are produced by ONE function. A second copy would drift
// inside a milestone, which is the argument `tests/render/README.md` already makes about
// `samples/03-first-light`.
//
// ================================================================================================
// WHAT IS THE DEVICE'S ANSWER AND WHAT IS THE HOST'S — SAID PLAINLY, BECAUSE THE PICTURE IS EVIDENCE
// ================================================================================================
//
// FROM THE DEVICE, per pixel: which surface is visible (`VisibilitySample::surface` and
// `::triangle`, settled by the compute rasteriser's 64-bit depth/payload atomic) and the resolved
// world normal (`VisbufferReadback::resolved`). Those two are what `virtual-geometry` produces and
// they are the only inputs a defect in the cluster hierarchy, the traversal or the visibility
// buffer can move.
//
// FROM THE HOST: the world POSITION of each pixel, reconstructed with `vg::reconstruct_surface()`
// from the same identity the device wrote; the shadow; and the lighting. The resolve does not
// publish a position — it publishes a normal and a u — so reconstructing it on the host is the
// only way to have one, and `frame.h` already makes the same argument for the illumination system.
// The reconstruction is not an independent scene: it reads the cooked asset the device rasterised,
// through the identity the device chose.
//
// ================================================================================================
// THE SHADOW IS A VIRTUAL SHADOW, THROUGH `src/rendering/shadows/` AND NOT AROUND IT
// ================================================================================================
//
// `virtual-shadows` is a page-residency model in this tree and, before this file, nothing in the
// repository ever rendered one of its pages. The sun's shadow here goes through the module's own
// machinery, in the order the module documents it:
//
//   1. `clipmap_level()` resolves each concentric level for the camera, SNAPPED to page boundaries.
//   2. Each receiver picks a level from its projected texel density (`clipmap_level_for`) and a
//      page from `address_of()` — so a distant receiver marks a coarser page than a near one.
//   3. `ShadowPageCache::request()` allocates physical slots, evicting under its own policy.
//   4. Pages the cache says need rendering are rasterised — once, from the caster set, into the
//      slot the cache handed out — and reported with `record_render()`.
//   5. Every lookup walks `resolve_shadow_lookup()`, so a page that could not be made resident
//      falls back to a coarser one, to a stale one, or to unshadowed, and the ledger counts which.
//   6. The comparison bias is `derive_shadow_bias()`'s, not a number this file picked.
//
// THE CASTER IS CLUSTER-GRANULAR AND CONSERVATIVE, AND THAT IS STATED RATHER THAN HIDDEN. What is
// rasterised into a page is the world bounding box of every LEVEL-0 cluster of every instance —
// virtual geometry's own finest unit — and the depth written is the box's FAR face along the light.
// Writing the near face would make every surface shadow itself; writing the far face makes the
// shadow slightly SHORT instead, by at most one cluster's depth, which is a visible-in-principle
// error that cannot be mistaken for acne. Rasterising the clusters' triangles would be the
// improvement and it is a shadow raster pass on the device, which is the work `virtual-shadows`
// still has open — see `src/rendering/shadows/README.md`.

#include <cy/core/memory/array.h>
#include <cy/rendering/shadows/cache.h>
#include <cy/rendering/shadows/clipmap.h>
#include <cy/rendering/shadows/fallback.h>

#include "frame.h"
#include "scene.h"

namespace cy::sample::fidelity {

/// An 8-bit RGBA frame, row-major from the top left, red in the low byte — the layout
/// `tests/render/golden.h`'s `Image` holds and a PNG row wants.
struct ShadedFrame {
    explicit ShadedFrame(Allocator& allocator) noexcept : texels(allocator) {}

    ShadedFrame(const ShadedFrame&) = delete;
    ShadedFrame& operator=(const ShadedFrame&) = delete;

    u32 width = 0;
    u32 height = 0;
    Array<u32> texels;
};

struct ShadeOptions {
    /// Physical shadow pages the whole frame may hold. The cache is shared across every light and
    /// clip level — `ShadowCacheConfig::slots`' own comment — so this is one number.
    u32 shadow_slots = 1024;
    /// Texels along one edge of a shadow page, and of a whole clip level. `page_texels` is the
    /// "measured platform decision" `address_space.h` reports rather than fixes.
    u32 page_texels = 64;
    u32 virtual_texels = 4096;
    u32 clipmap_levels = 8;
    /// World edge length of clip level 0.
    f32 first_level_extent = 16.0F;
    /// Exposure applied before the tone map. Chosen once, on the shot, and then fixed: an exposure
    /// tuned per run would make two captures incomparable.
    f32 exposure = 1.0F;
};

/// What shading the frame measured. Every field is a count or a ratio; nothing here is a judgement,
/// and the golden case asserts on several of them so that a picture cannot pass by being uniform.
struct ShadeReport {
    u32 covered = 0;
    /// Covered pixels whose reconstruction failed — the identity named a cluster or triangle the
    /// host could not decode. Non-zero is a defect, and the case says so.
    u32 unreconstructed = 0;
    /// Covered pixels that face the sun: the ones it reaches, and the ones a caster blocks. A
    /// pixel whose normal faces away is neither — it is counted separately, because "dark because
    /// it is turned away" and "dark because something is in front of it" are different claims and
    /// only the second is evidence about `virtual-shadows`.
    u32 lit_by_sun = 0;
    u32 shadowed = 0;
    u32 facing_away = 0;
    /// Distinct clusters decoded to reconstruct the frame — the visible set the device chose.
    u32 clusters_decoded = 0;

    u32 pages_requested = 0;
    u32 pages_rendered = 0;
    u32 pages_starved = 0;
    /// Shadow texels a caster was rasterised into. Zero means the shadow map is empty and every
    /// lookup would answer "lit" for a reason that has nothing to do with the scene.
    u32 shadow_texels_written = 0;
    /// Which rung of the fallback chain each lookup landed on.
    rendering::SubstitutionLedger substitutions;
    rendering::ShadowCacheStatistics cache;
};

/// Shade one captured frame. `capture` must carry the visibility samples, the resolved surfaces,
/// the visible cluster list and the view they were rendered with — `render_frames` fills all four
/// when it is passed a `Capture`.
[[nodiscard]] Status shade_frame(const Scene& scene, const Capture& capture,
                                 const ShadeOptions& options, ShadedFrame& out,
                                 ShadeReport& report) noexcept;

}  // namespace cy::sample::fidelity
