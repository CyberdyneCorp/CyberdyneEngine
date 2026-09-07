#pragma once
// Engine-side transform-gizmo geometry: where the handles are, in the frame they were drawn into.
// M7 task 5b.3, finishing M6's task 2.7.
//
// `editor-viewport-and-gizmos`:
//
//   "Gizmo geometry generation, depth behaviour, occlusion handling, and screen-constant sizing
//    SHALL be produced by **the engine**; the editor SHALL supply intent and manipulation state."
//
// and its forbidden-patterns list names "editor-side picking that does not match what the engine
// rendered". Those two together are this file: the editor sends what it WANTS manipulated, and the
// engine answers with where it PUT the handles, in the pixels of one identified frame.
//
// ================================================================================================
// WHAT WAS MISSING, AND WHY IT MATTERED MORE THAN IT LOOKED
// ================================================================================================
//
// M6 built both ends and nothing between them. `cy_editor_viewport::layout::GizmoLayout` is a
// READER — its own module says "there is no function in this module that produces a handle's
// position, and there is deliberately nowhere to put one" — `cy_editor_services::gizmo` carries the
// intent up and the layout down, and `cy_editor_protocol` has both messages. What nothing in
// `src/` did was produce a layout, so a click had nothing to land on however correct the reader
// was, and `docs/design/images/transform-gizmo.png` had been normative since M3 and realised in no
// pixel.
//
// ================================================================================================
// CONSTANT SCREEN SIZE IS AN INVARIANT HERE, NOT AN INTENTION
// ================================================================================================
//
// `editor-viewport-and-gizmos` requires "screen-constant sizing", and the failure it prevents is a
// gradual one: a gizmo drawn in world units becomes unusable exactly when precision matters most —
// when the camera is far away and every pixel is metres — and nobody files it, because at every
// individual moment it merely looks small.
//
// So the size is not a world length that happens to look right. `world_per_pixel` derives, from the
// view's own projection, the world distance one pixel spans AT THE PIVOT, and every handle is
// placed at a multiple of it. The consequence is a property a test can assert and the editor
// already checks from its side: `GizmoLayout::extent` is the SAME NUMBER at every camera distance,
// which is what `cy_editor_viewport::layout::screen_constant` refuses a runtime for getting wrong.
//
// ================================================================================================
// THE ENCODING IS THE EDITOR'S, BYTE FOR BYTE
// ================================================================================================
//
// `encode_gizmo_layout` writes exactly what `cy_editor_viewport::layout::GizmoLayout::decode`
// reads, and `decode_gizmo_intent` reads exactly what `cy_editor_services::gizmo::Request::encode`
// writes: little-endian throughout, `f32` by its bits, a `u32` count before a list. That is
// `cy_editor_core::codec`'s format, restated rather than shared because the two processes are two
// languages — and `cy_test_integration_editor_gizmo_wire` is what keeps the restatement true, by
// decoding this file's bytes with the editor's own decoder.
//
// ================================================================================================
// WHAT IS DELIBERATELY NOT HERE
// ================================================================================================
//
// THE APPEARANCE. Which handle is emphasised, what colour it is, how the active one lifts in
// luminance and saturation — `editor-visual-language` owns all of that and
// `cy_editor_visual::gizmo` implements it. This file answers WHERE, and says nothing about how it
// looks.
//
// THE MANIPULATION. Dragging a handle into a transform delta is
// `cy_editor_viewport::interaction`'s: it is manipulation state, which the specification assigns to
// the editor. This file is asked once per frame and holds nothing between calls.
//
// A RING AS A CURVE. `HandleSpot` is a disc, and a rotation ring is a circle. The published spot
// for a ring is the ring's point NEAREST THE CAMERA — the part drawn unoccluded, and the part a
// person aims at — with the editor's twelve-pixel acquisition slop around it. Hit-testing the whole
// curve would need a curve in the layout, and the layout is the editor's format; this is stated as
// a limitation rather than left to be discovered from a click that misses.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/transform.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/servers/render/model.h>

namespace cy::render {

/// Which manipulator the editor asked for.
///
/// The values are the wire codes `cy_editor_services::gizmo::mode_code` writes, so a change to
/// either side is a change to both and the compiler cannot help — which is what
/// `cy_test_integration_editor_gizmo_wire` is for.
enum class GizmoMode : u8 {
    Translate = 0,
    Rotate = 1,
    Scale = 2,
    Universal = 3,
    Count = 4,
};

[[nodiscard]] const char* gizmo_mode_name(GizmoMode mode) noexcept;

/// The frame the gizmo's axes are expressed in.
///
/// `Custom` carries a rotation with it on the wire; every other kind is a tag and nothing else.
enum class GizmoSpace : u8 {
    World = 0,
    Local = 1,
    Parent = 2,
    View = 3,
    Custom = 4,
    Count = 5,
};

/// What the manipulation happens about. Carried so that the layout is placed where the editor asked
/// rather than where this file would have chosen.
enum class GizmoPivot : u8 {
    Pivot = 0,
    Center = 1,
    Bounds = 2,
    Individual = 3,
    Count = 4,
};

/// One handle. **The order is the wire code**: it is the index in `cy_editor_viewport::gizmo::
/// Handle::ALL`, which is what `handle_code` writes and `handle_of_code` reads.
enum class GizmoHandle : u8 {
    AxisX = 0,
    AxisY,
    AxisZ,
    PlaneXY,
    PlaneYZ,
    PlaneZX,
    RingX,
    RingY,
    RingZ,
    /// The outer ring in the camera's plane: screen rotate.
    ScreenRing,
    BoxX,
    BoxY,
    BoxZ,
    /// The centre circle: free movement in the camera's plane.
    Screen,
    /// The centre cube: uniform scale.
    Uniform,
    Count,
};

[[nodiscard]] const char* gizmo_handle_name(GizmoHandle handle) noexcept;

/// What the editor asked for, decoded from `cy_editor_services::gizmo::Request`.
///
/// Every member is INTENT. There is no position, no extent and no handle in it, because those are
/// what this file answers with.
struct GizmoIntent {
    /// The frame the editor is showing. The layout must come back naming it, or the editor refuses
    /// the layout rather than hit-testing a click against a camera that has since moved.
    u64 frame_id = 0;
    GizmoMode mode = GizmoMode::Translate;
    GizmoSpace space = GizmoSpace::World;
    /// Only meaningful when `space` is `Custom`.
    Quat custom_space = Quat::identity();
    GizmoPivot pivot = GizmoPivot::Pivot;
    /// The engine's stable identities for what is selected. **Empty is a request, not an absence**:
    /// a selection that became empty must take the gizmo off the screen.
    Array<u64> identities;
    /// The size of the viewport the editor is asking about, in its own pixels, or zero.
    ///
    /// THE SPACE THE ANSWER HAS TO BE IN. A runtime renders at whatever size it renders at and the
    /// editor stretches that frame to fill its panel; a layout published in the frame's pixels
    /// would be hit-tested against a pointer in the panel's, and at 1280x720 into a 934x570 panel
    /// that is a handle missed by a third of the viewport — a drag that lands on nothing while both
    /// sides are individually correct.
    ///
    /// Zero means the editor did not say, and `rescale_gizmo_layout` is then not called: the answer
    /// is in the frame's own pixels, which is what a caller with nothing better to do should use.
    u32 viewport_width = 0;
    u32 viewport_height = 0;

    /// The camera the editor is asking to see the world from, when it said.
    ///
    /// **THE MOST IMPORTANT INTENT THERE IS**, and the one whose absence is hardest to attribute.
    /// Navigation is the editor's: a person orbits, pans and dollies. A runtime that renders its
    /// own camera while the editor does its manipulation arithmetic against a different one
    /// produces a gizmo drawn correctly, hit-tested correctly, and moving nothing — because the
    /// pivot sits at the editor's camera position and a screen-space axis of zero length has no
    /// direction. Every part is right and the drag reports "0.000 m".
    ///
    /// `fov_y_radians` of zero means the editor did not say — an orthographic viewport, or one
    /// older than this field — and a runtime reading zero renders its own view.
    f32 camera_position[3] = {0.0F, 0.0F, 0.0F};
    /// The camera's rotation, as a quaternion. It looks down its local −Z.
    f32 camera_rotation[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    f32 fov_y_radians = 0.0F;
    f32 near_plane = 0.0F;
};

/// Read what the editor sent.
///
/// Refuses a truncated message and an unknown enumerator rather than guessing, because a guessed
/// mode is a gizmo the user did not ask for and an unattributable one.
[[nodiscard]] Status decode_gizmo_intent(Span<const u8> bytes, GizmoIntent& out) noexcept;

/// One handle, where this frame put it.
struct GizmoHandleSpot {
    GizmoHandle handle = GizmoHandle::AxisX;
    /// Its centre in viewport pixels, origin at the viewport's top left — the same convention
    /// `project_to_pixel` produces and every windowing system reports a cursor in.
    f32 x = 0.0F;
    f32 y = 0.0F;
    /// The radius it was drawn at, in pixels. The editor adds its own acquisition slop.
    f32 radius = 0.0F;
    /// How near the camera it is, in view depth. Two overlapping handles resolve in favour of the
    /// smaller number, which is what "depth handling is the engine's" leaves the editor to honour.
    f32 depth = 0.0F;
};

/// The gizmo one frame drew.
struct GizmoLayout {
    u64 frame_id = 0;
    /// Which gizmo was drawn, which may not be the one asked for when the universal gizmo degraded.
    GizmoMode mode = GizmoMode::Translate;
    f32 centre_x = 0.0F;
    f32 centre_y = 0.0F;
    /// How far the gizmo reaches from its centre, in pixels. **The number that must not vary with
    /// camera distance.**
    f32 extent = 0.0F;
    Array<GizmoHandleSpot> spots;

    [[nodiscard]] bool empty() const noexcept { return spots.empty(); }
    /// Where one handle is, or null when this mode did not draw it.
    [[nodiscard]] const GizmoHandleSpot* spot(GizmoHandle handle) const noexcept;
};

/// Write the layout in the editor's encoding.
[[nodiscard]] Status encode_gizmo_layout(const GizmoLayout& layout, Array<u8>& out) noexcept;

/// The sizes a gizmo is drawn at, in logical pixels.
///
/// One place rather than constants scattered through the builder, because every one of them is a
/// number `docs/design/images/transform-gizmo.png` fixes and a reviewer compares against a picture.
struct GizmoStyle {
    /// How far the axis handles reach from the centre. The gizmo's `extent`.
    f32 extent_pixels = 88.0F;
    /// Where a plane handle sits along its two axes, as a fraction of `extent_pixels`.
    f32 plane_fraction = 0.38F;
    /// Where a scale box sits along its axis, as a fraction of `extent_pixels`. Inside the arrow
    /// tips, so the universal gizmo's box and arrow on one axis are separately targetable.
    f32 box_fraction = 0.72F;
    /// The outer screen-rotate ring, as a fraction of `extent_pixels`.
    f32 screen_ring_fraction = 1.18F;

    f32 axis_radius = 7.0F;
    f32 plane_radius = 9.0F;
    f32 ring_radius = 5.0F;
    f32 box_radius = 8.0F;
    /// The centre circle: free movement. Large, because it is what a person grabs to drag freely.
    f32 screen_radius = 15.0F;
    /// The centre cube: uniform scale. Drawn inside the circle and given the nearer depth, so the
    /// two stay separately targetable at the exact centre.
    f32 uniform_radius = 9.0F;

    /// How much nearer the camera the centre affordances are recorded as, so that a click at the
    /// exact centre resolves cube, then circle, then outer ring rather than by list order.
    /// `GizmoLayout::hit` on the editor's side orders by distance and then by depth.
    f32 centre_depth_bias = 0.02F;
};

/// The world distance one viewport pixel spans at `world_point`, for this view.
///
/// THE FUNCTION SCREEN-CONSTANT SIZING IS. Perspective: the view frustum's height at that depth,
/// divided by the viewport's height. Orthographic: the projection's own height, divided by the
/// same. Zero when the view has no viewport or the point is degenerate, which the builder treats as
/// "draw nothing" rather than as an enormous gizmo.
[[nodiscard]] f32 world_per_pixel(const View& view, Vec3 world_point) noexcept;

/// Build the layout for one selection, in one frame.
///
/// `pivot` is where the gizmo sits and `axes` is the rotation its X, Y and Z mean — the caller
/// resolves both from the intent and the world, because this module has no scene. A pivot behind
/// the camera or outside a degenerate view produces an EMPTY layout naming the frame, which is a
/// legitimate answer the editor already handles: it takes the gizmo off the screen.
[[nodiscard]] GizmoLayout build_gizmo_layout(const View& view, Vec3 pivot, Quat axes,
                                             GizmoMode mode, u64 frame_id,
                                             const GizmoStyle& style = {}) noexcept;

/// Move a layout from the pixels of one rectangle into the pixels of another.
///
/// What `GizmoIntent::viewport_width` exists for. Positions scale per axis, because the two
/// rectangles may differ in aspect and a handle has to land where it is drawn. Radii and the extent
/// scale by the SMALLER of the two factors, because a radius is one number and a disc that grew on
/// one axis only would be an ellipse the hit test does not model — and the smaller factor is the
/// conservative choice, which keeps a handle inside what was drawn rather than outside it.
///
/// Does nothing when either size is zero or the layout is empty.
void rescale_gizmo_layout(GizmoLayout& layout, u32 from_width, u32 from_height, u32 to_width,
                          u32 to_height) noexcept;

/// The axes a space means, given the object's own rotation and the view.
///
/// `Parent` is answered by the caller passing the parent's rotation as `local`, because this module
/// has no hierarchy; `Custom` is the intent's own quaternion.
[[nodiscard]] Quat gizmo_axes(const GizmoIntent& intent, const View& view, Quat local) noexcept;

}  // namespace cy::render
