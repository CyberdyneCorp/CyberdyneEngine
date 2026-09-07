#pragma once
// Precise invalidation: previous and current bounds, projected, and nothing else dirtied. Task 8.1.
//
// `virtual-shadows` — "Precise invalidation" and "Deformation and cache validity".
//
// ================================================================================================
// THE WHOLE REQUIREMENT IS "ONLY THE OVERLAPPING PAGES"
// ================================================================================================
//
// "When a caster moves, its previous and current bounds — already held per instance in the GPU
// scene — SHALL be projected into the shadow spaces of affected lights, and only the overlapping
// pages SHALL be marked dirty. Moving one object SHALL NOT invalidate a light's whole shadow
// space."
//
// So `invalidate_caster_motion()` is two calls to `pages_covering()` and nothing else, and the
// character-walking-across-a-city case asserts the count is a handful rather than the level. The
// previous bounds are as load-bearing as the current ones: dirtying only where the caster now is
// leaves its old shadow painted on the world, which is the bug this shape of the interface makes
// hard to write.
//
// ================================================================================================
// THE DEFORMATION MODE IS DECLARED BY CONTENT, AND `AlwaysDirty` IS REPORTABLE ON PURPOSE
// ================================================================================================
//
// Wind, vertex animation and skinning change a shape without changing a transform. Four declared
// modes decide what that costs, and the specification singles out the last one — "`AlwaysDirty`
// SHALL be reportable per asset, since it is the mode that silently removes the benefit of
// caching". `InvalidationReport` counts it per call and the caller accumulates per asset.
//
// `Bounded` is the interesting one: the caster's bounds are expanded by its declared deformation
// envelope, so a swaying tree dirties nothing while it stays inside the envelope it declared. That
// is the forest scenario, and it is why `CasterMotion` carries an envelope rather than the renderer
// guessing a worst case.

#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/rendering/shadows/address_space.h>
#include <cy/rendering/shadows/cache.h>

namespace cy::rendering {

/// `virtual-shadows`' four shadow deformation modes, in the order its table lists them.
enum class ShadowDeformationMode : u8 {
    /// Never dirties by deformation.
    Static = 0,
    /// Bounds expanded to cover the deformation envelope; dirties only when it moves.
    Bounded,
    /// Dirties pages each frame it is visible in.
    Dynamic,
    /// Never cached; rendered every frame.
    AlwaysDirty,
    Count,
};

[[nodiscard]] const char* shadow_deformation_mode_name(ShadowDeformationMode mode) noexcept;

/// One caster's frame-to-frame change, as the GPU scene already holds it.
struct CasterMotion {
    u64 instance_id = 0;
    Aabb previous;
    Aabb current;
    ShadowDeformationMode mode = ShadowDeformationMode::Static;
    /// World-space margin covering the declared deformation envelope, including a displacing
    /// material's declared maximum displacement. Applied under `Bounded`; ignored otherwise.
    f32 envelope = 0.0F;
};

struct InvalidationReport {
    u32 pages_dirtied = 0;
    /// Casters that dirtied nothing because they did not move outside their declared envelope.
    u32 casters_unchanged = 0;
    /// Casters in `AlwaysDirty`. The number that says caching has been switched off by content.
    u32 always_dirty_casters = 0;
    /// Pages the projection wanted to name and the caller's scratch could not hold. Non-zero means
    /// a caster covering more of the light than the scratch budgeted for; reported, not hidden.
    u32 overflow = 0;
};

/// Scratch the projections write into. Supplied by the caller so this function allocates nothing on
/// a frame path; sixty-four pages is a generous ceiling for one caster in one light.
struct InvalidationScratch {
    VirtualPage* pages = nullptr;
    u32 capacity = 0;
};

/// Dirty the pages one caster's motion touches, in one level of one light.
[[nodiscard]] InvalidationReport invalidate_caster_motion(ShadowPageCache& cache,
                                                          const ShadowAddressSpace& space,
                                                          const CasterMotion& motion,
                                                          InvalidationScratch scratch) noexcept;

/// Dirty everything a light owns, because the light moved. Attributed to the light, which is the
/// "a moving light is attributable" scenario — the cost shows up against a named light rather than
/// as an unexplained rise in renders.
[[nodiscard]] InvalidationReport invalidate_light(ShadowPageCache& cache, u32 light_slot,
                                                  u64 light_id) noexcept;

/// Dirty only the pages a world cell's bounds project into, attributed to the cell.
[[nodiscard]] InvalidationReport invalidate_streaming(ShadowPageCache& cache,
                                                      const ShadowAddressSpace& space,
                                                      const Aabb& cell_bounds, u64 cell_id,
                                                      InvalidationScratch scratch) noexcept;

}  // namespace cy::rendering
