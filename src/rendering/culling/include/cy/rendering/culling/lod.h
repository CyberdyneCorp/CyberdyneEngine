#pragma once
// Level of detail, visibility ranges and HLOD. Task 4.4.3.
//
// `rendering-culling-and-lod` — "Level of detail": discrete LOD chains are "selected from projected
// screen coverage, adjusted by a global LOD bias, a per-instance bias, and the view's quality
// setting", with "hysteresis to prevent oscillation at the boundary" and a "dithered cross-fade
// over a configurable distance band". Shadow and reflection views carry their own bias.
//
// ================================================================================================
// COVERAGE, NOT DISTANCE — AND WHY THE FORMULA IS WRITTEN OUT
// ================================================================================================
//
// A distance threshold is wrong whenever the field of view or the resolution changes, and both
// change: a scope, an ultrawide display, a half-resolution reflection view. Coverage is the
// fraction of the viewport's HEIGHT a bounding sphere subtends, so it is already correct for all
// three, and `render::MeshLod::screen_coverage_threshold` is authored in it.
//
// For a perspective view the projected height of a sphere of radius r at distance d is
//
//     coverage = r / (d * tan(fov_y / 2))
//
// in units of the viewport height. The distance is measured along the VIEW DIRECTION rather than as
// a Euclidean distance to the camera, so an object at the edge of a wide frame does not silently
// drop a level relative to the same object at the centre.
//
// ================================================================================================
// HYSTERESIS IS A PROPERTY OF THE PAIR, NOT OF THE INSTANCE
// ================================================================================================
//
// An instance sitting exactly on a threshold flips level every frame as the camera breathes, and
// the flip is visible because the two levels differ. The fix is to require coverage to fall BELOW
// `threshold * (1 - hysteresis)` to coarsen and to rise ABOVE `threshold` to refine — an asymmetric
// band whose width is the same for every instance and whose state is the level chosen last frame.
// That previous level is the caller's, passed in and returned, because this module holds no
// per-instance state: a scene of a million instances would otherwise need a second array of them
// here, parallel to the one the caller already has.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/servers/render/mesh.h>

namespace cy::rendering {

inline constexpr u32 kInvalidLod = ~0U;

/// The quality knobs a view applies to every LOD decision it makes.
///
/// `view_bias` is what makes a shadow view coarser than the main one: `rendering-culling-and-lod`
/// requires shadow and reflection views to "use their own bias so lower LODs are used where detail
/// is not visible", and a negative bias is what expresses that.
struct LodSettings {
    /// Global quality bias. Positive keeps more detail.
    f32 global_bias = 0.0F;
    /// This view's own bias, added to the global one. Negative for shadow and reflection views.
    f32 view_bias = 0.0F;
    /// Fraction of a threshold the coverage must fall below before a level is coarsened. Zero
    /// disables hysteresis, which is what a deterministic test wants.
    f32 hysteresis = 0.1F;
    /// Width of the cross-fade band, as a fraction of the threshold. Inside it both levels are
    /// drawn with complementary dither masks and temporal antialiasing resolves the blend.
    f32 cross_fade_band = 0.0F;
};

/// What one instance's LOD decision came out as.
struct LodSelection {
    u32 level = 0;
    /// The level being faded towards, or `kInvalidLod` when there is no transition in flight.
    u32 fade_to = kInvalidLod;
    /// 0 at the start of the band and 1 at its end. The dither threshold the shader compares
    /// against, and zero whenever `fade_to` is invalid.
    f32 fade = 0.0F;
};

/// The fraction of the viewport height a bounding sphere subtends. See the header comment.
///
/// `view_depth` is the distance along the view direction and must be positive; a non-positive depth
/// means the sphere's centre is at or behind the eye, which returns 1 — an object the camera is
/// inside covers the screen, and returning 0 there would drop it to its coarsest level exactly when
/// it fills the frame.
[[nodiscard]] f32 screen_coverage(f32 radius, f32 view_depth, f32 fov_y_radians) noexcept;

/// The same for an orthographic view, where coverage does not depend on distance at all.
[[nodiscard]] f32 screen_coverage_orthographic(f32 radius, f32 ortho_height) noexcept;

/// Select a level from a chain, given the level chosen last frame.
///
/// `previous` is `kInvalidLod` for an instance seen for the first time, which takes the plain
/// threshold with no hysteresis — the band exists to stop oscillation, and there is nothing to
/// oscillate away from yet.
///
/// The chain's thresholds descend (`render::MeshLod`), and a mesh too small for any threshold is
/// drawn at its coarsest level rather than dropped: `select_lod` in the render server says so and
/// this function keeps the same answer.
[[nodiscard]] LodSelection select_lod(Span<const render::MeshLod> chain, f32 coverage,
                                      f32 instance_bias, const LodSettings& settings,
                                      u32 previous) noexcept;

// --- Visibility ranges and HLOD ----------------------------------------------------------------

/// How an instance leaves and enters visibility at its range bounds.
enum class FadeMode : u8 {
    /// A hard switch at the bound.
    None = 0,
    /// The instance fades its own alpha over the margin.
    Self,
    /// Parent and children cross-fade, both drawn during the transition. What an HLOD swap does.
    Dependents,
};

/// `rendering-culling-and-lod` — "Visibility ranges and HLOD". An instance may declare a range and
/// a visibility PARENT, "forming a hierarchy in which a parent's visibility replaces its
/// children's".
struct VisibilityRange {
    /// Metres. `end` of zero means "no upper bound", so a zeroed struct is "always visible".
    f32 begin = 0.0F;
    f32 end = 0.0F;
    /// Metres over which the instance fades in at `begin` and out at `end`.
    f32 fade_margin = 0.0F;
    FadeMode mode = FadeMode::None;
    /// The slot of the instance whose visibility replaces this one's, or
    /// `kInvalidVisibilityParent`.
    u32 parent = ~0U;
};

inline constexpr u32 kInvalidVisibilityParent = ~0U;

/// How visible one instance is at a distance, ignoring its hierarchy: 0 fully faded out, 1 fully
/// in.
[[nodiscard]] f32 visibility_range_alpha(const VisibilityRange& range, f32 distance) noexcept;

/// One resolved instance of a visibility hierarchy.
struct HlodResolution {
    /// Whether the instance is drawn at all this frame. False for a child whose parent has taken
    /// over, and for an instance outside its own range.
    bool visible = false;
    /// The alpha it is drawn at. Between 0 and 1 only during a `Dependents` cross-fade, where both
    /// the parent and its children are drawn.
    f32 alpha = 0.0F;
};

/// Resolve a whole visibility hierarchy in one pass.
///
/// `ranges` and `distances` are parallel arrays indexed by slot; `out` is written to the same
/// length. A parent's `parent` field points at ITS parent, so a chain of proxies resolves to
/// "exactly one visible level per branch" — which is the nested-HLOD scenario, and it is why this
/// is a function over the whole array rather than a per-instance query: answering one instance
/// requires walking to the root anyway, and doing that per instance is quadratic on a deep chain.
///
/// Fails with `InvalidArgument` when the parent links contain a cycle, which is a content error the
/// importer should have caught and which would otherwise hang the frame.
[[nodiscard]] Status resolve_hlod(Span<const VisibilityRange> ranges, Span<const f32> distances,
                                  Span<HlodResolution> out) noexcept;

}  // namespace cy::rendering
