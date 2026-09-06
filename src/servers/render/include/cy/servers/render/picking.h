#pragma once
// Engine-side picking: what is picked is what was drawn. Task 4.2.
//
// `editor-viewport-and-gizmos` — "Selection and picking": "Picking SHALL be **engine-side**, so
// that what is picked matches what is rendered — including virtual geometry, instanced content,
// foliage, terrain, and skinned meshes", and its forbidden-patterns list names "editor-side picking
// that does not match what the engine rendered".
//
// ================================================================================================
// THE ONE DECISION THAT MAKES THAT TRUE RATHER THAN INTENDED
// ================================================================================================
//
// A pick RESOLVES AGAINST THE DRAW LIST, not against the scene. `pick_ray`, `pick_rect` and
// `pick_polygon` all take the `Span<const DrawItem>` that `RenderServer::collect_draws` produced
// for the view, and consider nothing else. So the candidate set is, by construction, a SUBSET of
// what the renderer drew:
//
//   * an instance culled by the frustum is not in the list and cannot be picked;
//   * an instance whose layer the view does not draw is not in the list and cannot be picked;
//   * an instance hidden by `kInstanceVisible` is not in the list and cannot be picked;
//   * an instance published by a producer this file has never heard of — a foliage batch, a mesh
//     particle, a virtual-geometry cluster — IS in the list, and is therefore pickable with no code
//     here knowing what produced it.
//
// That last clause is the requirement about instanced, skinned and virtualised content, and it is
// answered by reading the same records the draw did rather than by a case per producer. The
// alternative — walking the scene again with its own visibility rules — is the shape of defect the
// requirement exists to prevent: two traversals that agree on the day they are written.
//
// A pick therefore CANNOT be resolved without a frame having been collected, and that is deliberate
// too: `editor-viewport-and-gizmos` requires that "WHEN the user clicks in a streamed viewport THEN
// the hit SHALL be resolved against the view state of the frame shown, not a newer one". The `View`
// passed here is the one the transport carried alongside the image (`viewport_transport.h`), and
// the draw list is that frame's. Passing a newer view with an older list is possible and is exactly
// what the frame identifier on `PresentedViewportFrame` exists to stop a caller doing by accident.
//
// ================================================================================================
// WHAT IS DELIBERATELY NOT HERE
// ================================================================================================
//
// TRIANGLE PRECISION. A `GpuInstance` carries a world-space bounding sphere and a 4x3 transform,
// and no local box — see gpu_scene.h, where the sphere is four floats rather than six on purpose.
// So a hit here is a hit against the bounding volume the culler used, ordered by distance along the
// ray and, at equal distance, by how near the ray passed the centre. That is enough to select, it
// is deterministic, and it costs nothing per frame; a GPU-side identifier buffer written by the
// same pass that shaded the pixel is the exact answer and belongs with the render graph at M6,
// where there is a pass to write it from. Stated here rather than discovered: overlapping bounding
// volumes produce several candidates, which is why cycling exists and is a requirement rather than
// a nicety.
//
// PREFABS. "selecting the prefab root or the inner instance explicitly" is a document question —
// the engine has no prefabs, only instances with stable identities — so it is answered in the
// editor, which owns the hierarchy. What this file supplies is the stable identity of what was
// drawn; which authoring object owns it is `cy_editor_viewport::picking`'s.
//
// LOCKED OBJECTS. Locking is an authoring state, so the editor supplies the excluded identities in
// `PickFilter::excluded`. That is the editor deciding what should be pickable and the engine
// resolving it, which is this capability's division of labour rather than a compromise of it.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/memory/array.h>
#include <cy/servers/render/model.h>
#include <cy/servers/render/sort.h>

namespace cy::render {

/// Where a pixel of the presented frame sends a ray, in world space.
///
/// Built from the frame's own view state, so it is the ray the user aimed rather than the ray the
/// editor's current camera would produce. `pixel_x` and `pixel_y` are in the view's viewport rect,
/// measured from its top-left corner in the target's pixels — the convention every windowing system
/// reports a cursor in, so a caller does not flip it and the one place the flip happens is here.
[[nodiscard]] Ray ray_through_pixel(const View& view, f32 pixel_x, f32 pixel_y) noexcept;

/// A rectangle of the presented frame's pixels, in the same convention as `ray_through_pixel`.
struct PickRect {
    f32 min_x = 0.0F;
    f32 min_y = 0.0F;
    f32 max_x = 0.0F;
    f32 max_y = 0.0F;

    [[nodiscard]] constexpr bool contains(f32 x, f32 y) const noexcept {
        return x >= min_x && x <= max_x && y >= min_y && y <= max_y;
    }
};

/// What the editor will and will not accept as a hit.
///
/// Every member is editor INTENT. None of it re-implements a visibility rule the renderer already
/// applied: a hidden or culled instance never reaches this filter, because it never reached the
/// draw list.
struct PickFilter {
    /// Identities the editor will not select — locked objects, and the object being dragged when a
    /// vertex snap is looking for something to snap to. Compared against `DrawItem::stable_id`.
    Span<const u64> excluded;

    /// Whether a transparent surface can be hit at all.
    ///
    /// `editor-viewport-and-gizmos`: "selecting through transparent surfaces by intent". The intent
    /// is a modifier the user holds, so it is a field rather than a policy: off, a click passes
    /// through glass and selects what is behind it; on, the glass is selectable like anything else.
    /// The transparency of a candidate is read from the SORT LAYER THE RENDERER USED, so this
    /// cannot disagree with how the surface was actually drawn.
    bool include_transparent = true;

    /// Cap on how many candidates are RETURNED. Zero means "no cap".
    ///
    /// The cap is applied after ordering, never during collection, so the three candidates a caller
    /// asks for are the three nearest rather than the first three the draw list happened to hold.
    /// A cap applied during collection would make the answer depend on publication order, which is
    /// the property `sort.h` spends a header explaining why nothing may depend on.
    u32 max_candidates = 0;
};

/// One thing the pick found.
struct PickCandidate {
    /// What was drawn. The identity the editor selects by, and the one that survives streaming —
    /// `editor-viewport-and-gizmos` requires selection to be "expressed in stable identity".
    u64 stable_id = 0;
    /// Where the record lives in the GPU scene, for a caller that wants to read it back this frame.
    /// NEVER an identity: it is allocation order, and it is not carried into a selection.
    u32 instance_slot = 0;
    /// Which surface of the instance's mesh produced the draw that was hit.
    u32 surface = 0;
    /// Distance along the ray to the bounding volume's entry point, in world units. Zero for a
    /// rectangle or polygon pick, where there is no ray.
    f32 distance = 0.0F;
    /// How near the ray passed the bounding volume's centre, as a fraction of its radius: 0 dead
    /// centre, 1 at the silhouette. The tie-break that makes a click on two concentric objects
    /// prefer the one the cursor is actually over. Zero for an area pick.
    f32 centrality = 0.0F;
    /// Whether the renderer drew this in the transparent layer. Read from the draw's sort key.
    bool transparent = false;
};

/// Candidates under a ray, nearest first.
///
/// ORDERED BY `(distance, centrality, stable_id)`, which is a TOTAL order for the same reason
/// `sort_draws` is: two candidates can share a distance and a centrality, and no two can share a
/// stable identity. So cycling through overlapping candidates visits them in the same sequence
/// every time, on every machine — a cycle whose order depended on slot allocation would put a
/// different object under the second click depending on what had been created and destroyed earlier
/// in the session, which is the kind of defect a user reports as "selection is random".
///
/// `out` is cleared first. Reports `ErrorCode::OutOfMemory` if it cannot grow, and nothing else:
/// a pick that finds nothing is an empty list rather than a failure, because "the user clicked the
/// sky" is not an error.
[[nodiscard]] Status pick_ray(Span<const GpuInstance> records, Span<const DrawItem> drawn,
                              const Ray& ray, const PickFilter& filter,
                              Array<PickCandidate>& out) noexcept;

/// Candidates whose bounding volume's centre projects inside `rect`.
///
/// Rectangle (marquee) selection. Ordered by stable identity, because there is no depth to order by
/// and publication order is what determinism forbids.
[[nodiscard]] Status pick_rect(Span<const GpuInstance> records, const View& view,
                               Span<const DrawItem> drawn, const PickRect& rect,
                               const PickFilter& filter, Array<PickCandidate>& out) noexcept;

/// Candidates whose bounding volume's centre projects inside the polygon `vertices` encloses.
///
/// Lasso selection. The polygon is in the same pixel convention as `PickRect`, must have at least
/// three vertices, and may be concave; containment is the even-odd rule. Reports
/// `ErrorCode::InvalidArgument` for fewer than three vertices, because a lasso of two points is a
/// caller mistake rather than an empty selection.
[[nodiscard]] Status pick_polygon(Span<const GpuInstance> records, const View& view,
                                  Span<const DrawItem> drawn, Span<const Vec2> vertices,
                                  const PickFilter& filter, Array<PickCandidate>& out) noexcept;

/// The candidate a click selects, given how many times the user has clicked the same spot.
///
/// `editor-viewport-and-gizmos`: "cycling through overlapping candidates". Returns the null
/// identity when there are no candidates. `cycle` is unbounded and wraps, so a caller counts clicks
/// rather than tracking a position in a list that may have changed length since the last click.
[[nodiscard]] u64 cycle_candidate(Span<const PickCandidate> candidates, u32 cycle) noexcept;

/// Where a world-space point lands in the presented frame's pixels, in `ray_through_pixel`'s
/// convention. Returns false when the point is behind the camera, where there is no answer — a
/// projection that wrapped a point behind the eye onto the screen is how an overlay ends up drawn
/// at the wrong end of the viewport.
[[nodiscard]] bool project_to_pixel(const View& view, Vec3 world, Vec2& out_pixel) noexcept;

}  // namespace cy::render
