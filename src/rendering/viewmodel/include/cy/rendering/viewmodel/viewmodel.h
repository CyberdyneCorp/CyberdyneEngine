// SPDX-License-Identifier: MIT
#pragma once
// The first-person WEAPON VIEW: the arms and the weapon, drawn with their own field of view and
// near plane into the world's frame. `camera-system` — "One camera, several views".
//
// ================================================================================================
// THE ONE PROBLEM, AND WHY THE ANSWER IS A DEPTH RANGE RATHER THAN A SECOND FRAME
// ================================================================================================
//
// A weapon held at arm's length is half a metre long and the player can walk his face into a wall.
// Drawn as world geometry, the barrel goes into the wall and disappears. The two usual answers are
// both worse than the problem: shrinking the weapon until it fits inside the player's capsule makes
// it a different object to light, and drawing it in a second frame over the first gives it no
// shadows, no fog, no GI and no motion vectors anything temporal agrees with.
//
// So the weapon view is drawn INTO THE WORLD'S FRAME — one depth buffer, one prepass, one light
// loop, one temporal resolve — and differs in exactly one thing: the matrix its vertices are pushed
// through. `viewmodel_projection` is the weapon's own perspective (its field of view, a near plane
// a few millimetres out) with its depth SQUEEZED into the slice of the reversed-Z range nearest the
// eye: `z' = (1 - s) + s·z`, the `s = depth_fraction`. Every world surface past
// `world_near / (1 - s)` has a depth below `1 - s`, so every weapon fragment is in front of it —
// `guaranteed_clear_distance` is that number — and the weapon keeps its own depth order inside the
// slice, so it still hides itself correctly.
//
// ================================================================================================
// WHAT IS STILL THE WORLD'S
// ================================================================================================
//
// The position the forward pass lights. A weapon's instance row is where the weapon IS in the world
// — at the camera, offset to the right hand — and the vertex stage hands that position to the
// fragment stage unchanged, so the weapon is lit by the lights around the player, samples the
// world's shadow map where it is (`ViewmodelShadowPolicy::receive`), is fogged by the fog between
// the eye and it, and takes the irradiance volume's light at its own position. Only the pixel it
// lands on is the viewmodel's, and the frame's light lists are found from the world's projection of
// that position (`cy/frame.slang`'s `frameClusterPixel`), not from the pixel.
//
// ================================================================================================
// MOTION
// ================================================================================================
//
// `ViewmodelFrame` keeps last frame's viewmodel projection, and the previous rows are that
// projection over the camera's previous view rebased by its displacement — the world's own rule
// (`previous_camera_relative_view`). A weapon carried by a turning camera therefore has a motion
// vector of zero, which is what lets the temporal resolve keep its history instead of smearing it,
// and a weapon raised to the eye as its field of view narrows moves the way it was drawn to move.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/rendering/assembly/frame_assembly.h>
#include <cy/rendering/pipeline/frame_pipelines.h>
#include <cy/rendering/temporal/framework.h>
#include <cy/servers/render/model.h>

namespace cy::rendering::viewmodel {

/// `FrameViewData::viewmodel_control[0]`'s bit, `cy/frame.slang`'s `kCyViewmodelContactShadows`.
inline constexpr u32 kViewmodelContactShadows = 1U;

/// The weapon view's far plane when the view that described it had none. A weapon is at arm's
/// length; ten metres is room for a rifle's muzzle flash and still a finite plane to cull against.
inline constexpr f32 kDefaultViewmodelFar = 10.0F;

/// The weapon view's part in the world's shadows. `camera-system` asks for a policy rather than a
/// rule, because both answers are right for some game.
struct ViewmodelShadowPolicy {
    /// The weapon samples the world's shadow map at its world position: it darkens in the shade of
    /// the wall it is held against. On by default.
    bool receive = true;
    /// The weapon is drawn into the world's shadow map from where it is in the world. Off by
    /// default: a floating pair of arms casting onto the floor in front of the player is the
    /// artefact most games turn this off for, and the weapon then never shadows itself either.
    bool cast = false;
    /// The weapon takes the screen-space contact term as well as the map's. Off by default: its
    /// depth is squeezed against the near plane, so the trace finds the weapon as its own occluder.
    bool contact = false;
};

/// What the weapon view is: which layers it draws, through what lens, and its shadow policy.
struct ViewmodelSettings {
    /// The layers the weapon view draws — and the world view therefore does not. Zero is no weapon
    /// view, which is what a frame without one passes.
    render::LayerMask layers = render::kNoLayers;
    /// The weapon's own lens. 55 degrees is a common viewmodel field of view: narrow enough that
    /// the weapon is not stretched at the edge of a wide world lens.
    f32 vertical_fov_radians = 0.95993109F;
    /// Metres. A few millimetres, so a weapon pressed to the eye is still drawn.
    f32 near_plane = 0.01F;
    /// Metres, finite: the weapon view is culled against it.
    f32 far_plane = kDefaultViewmodelFar;
    /// The slice of the reversed-Z range nearest the eye the weapon's depth is squeezed into. See
    /// the header comment; `guaranteed_clear_distance` is what it buys.
    f32 depth_fraction = 1.0F / 16.0F;
    ViewmodelShadowPolicy shadows;

    [[nodiscard]] bool enabled() const noexcept { return layers != render::kNoLayers; }
};

/// Refuses a lens that is not one — a field of view outside (0, pi), a near plane that is not
/// positive, a far plane that is not finite and beyond it — and a depth fraction outside (0, 0.5].
[[nodiscard]] Status validate(const ViewmodelSettings& settings) noexcept;

/// Squeeze a reversed-Z projection's depth into `[1 - fraction, 1]`: `z' = (1 - s)·w + s·z` in clip
/// space, which is `z_ndc' = (1 - s) + s·z_ndc`. The x, y and w rows are untouched, so the weapon
/// lands on exactly the pixels its own projection put it on.
[[nodiscard]] Mat4 compress_depth(const Mat4& projection, f32 fraction) noexcept;

/// The weapon view's projection at `aspect`: its own lens, built by `render::Projection::matrix` —
/// the renderer's one construction site — and, with `compressed`, squeezed. The uncompressed form
/// is what the weapon view is culled with.
[[nodiscard]] Mat4 viewmodel_projection(const ViewmodelSettings& settings, f32 aspect,
                                        bool compressed = true) noexcept;

/// The distance past which every world surface is behind every weapon fragment:
/// `world_near / (1 - fraction)`. A wall nearer than that is nearer than the world's own near plane
/// by a few per cent, which a player's collision does not allow.
[[nodiscard]] f32 guaranteed_clear_distance(f32 world_near_plane, f32 fraction) noexcept;

/// The weapon view from the two render views one evaluated camera produced —
/// `camera::produce_weapon_view` and the primary view beside it. Refuses a weapon view whose
/// purpose is not `ViewPurpose::Viewmodel`, two views that are not one camera's pose, two views
/// that share a history identity (their temporal histories must stay apart), a weapon view that is
/// not perspective or draws no layer.
[[nodiscard]] Expected<ViewmodelSettings, Error> settings_from_views(
    const render::ViewDescription& primary, const render::ViewDescription& weapon,
    const ViewmodelShadowPolicy& shadows) noexcept;

/// What the frame assembly needs of the weapon view: its layers, its cull frustum — world space,
/// from `world_view` and the UNCOMPRESSED viewmodel projection — and its shadow policy. A disabled
/// `settings` gives the default, which is no weapon view.
[[nodiscard]] assembly::AssemblyViewmodel assembly_view(const ViewmodelSettings& settings,
                                                        const Mat4& world_view,
                                                        f32 aspect) noexcept;

/// This frame's viewmodel rows and last frame's, both camera-relative and unjittered.
struct ViewmodelMatrices {
    Mat4 relative_to_clip = Mat4::identity();
    Mat4 previous_relative_to_clip = Mat4::identity();
};

/// The weapon view across frames: last frame's projection, so a change of lens has motion.
///
/// One per weapon view, stepped once per frame AFTER `FrameAssembly::assemble`, whose temporal
/// framework holds the camera's current and previous views by then.
class ViewmodelFrame {
public:
    /// This frame's matrices. `relative_view` is the camera's rotation alone — the matrix the
    /// world's rows are drawn through. On the first frame and after `reset` — a weapon swapped for
    /// another — last frame's projection is this frame's, so the lens has no motion.
    [[nodiscard]] ViewmodelMatrices advance(const ViewmodelSettings& settings, f32 aspect,
                                            const Mat4& relative_view,
                                            const TemporalFramework& temporal) noexcept;
    void reset() noexcept { has_previous_ = false; }
    [[nodiscard]] bool has_previous() const noexcept { return has_previous_; }

private:
    Mat4 previous_projection_ = Mat4::identity();
    bool has_previous_ = false;
};

/// Write the weapon view's words of the view block: this frame's rows with the frame's jitter —
/// the same sub-pixel offset `pipeline::upload_for` puts on the world's, `jitter` in pixels of the
/// `width` by `height` render extent — last frame's unjittered, and the policy bits.
void write_viewmodel(const ViewmodelMatrices& matrices, const ViewmodelSettings& settings,
                     Vec2 jitter, u32 width, u32 height, pipeline::FrameViewData& out) noexcept;

}  // namespace cy::rendering::viewmodel
